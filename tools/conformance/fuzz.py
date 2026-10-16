"""Deterministic differential fuzz cases for the x86 lifter.

This module generates *safe* short instruction sequences and feeds them through
the existing conformance runner.  The native 32-bit x86 execution remains the
oracle; fuzzing only varies instruction combinations and inputs.

Examples::

    py -3 -m tools.conformance.fuzz
    py -3 -m tools.conformance.fuzz --count 250 --seed 0xC0FFEE
    py -3 -m tools.conformance.fuzz --count 1000 --seed 7 --keep -v

The generator deliberately avoids privileged instructions, memory operands,
division and other operations that can fault.  Cases are reproducible from the
printed seed and case index, so a failure can be promoted into ``cases.py`` as a
permanent regression test.
"""

from __future__ import annotations

import argparse
import random
import sys
from typing import Iterable

from .cases import Case

# Boundary-heavy values make narrow-width and flag bugs much easier to hit than
# uniform random 32-bit inputs alone.
_EDGES = (
    0x00000000,
    0x00000001,
    0x00000002,
    0x0000007F,
    0x00000080,
    0x000000FF,
    0x00000100,
    0x00007FFF,
    0x00008000,
    0x0000FFFF,
    0x00010000,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFE,
    0xFFFFFFFF,
    0x12345678,
    0x87654321,
)

_WIDTHS = {
    8: ("al", "cl"),
    16: ("ax", "cx"),
    32: ("eax", "ecx"),
}

_CONDS = ("z", "nz", "b", "ae", "be", "a", "s", "ns", "l", "ge", "le", "g")
# The conformance snippet runner only enables explicit carry tracking when the
# sequence contains ADC/SBB.  Do not manufacture SETcc carry consumers here or
# a correct arithmetic lift can look wrong merely because the harness did not
# request CF bookkeeping.  Carry itself is fuzzed by the dedicated ADC/SBB
# families below.
_NON_CARRY_CONDS = ("z", "nz", "s", "ns", "l", "ge", "le", "g")
_FLAG_PRESERVING_NOISE = (
    "mov edx, edx",
    "mov esi, esi",
    "lea edi, [edi]",
)


def _inputs(rng: random.Random, n: int = 20) -> list[tuple[int, int]]:
    """Return deterministic boundary-biased (eax, ecx) inputs."""
    out: list[tuple[int, int]] = []
    # Guarantee zero/nonzero, signed boundaries and carry-heavy pairs first.
    forced = (
        (0, 0),
        (0, 1),
        (1, 0),
        (0x7FFFFFFF, 1),
        (0x80000000, 1),
        (0xFFFFFFFF, 1),
        (0x00000080, 0x000000FF),
        (0x00008000, 0x0000FFFF),
    )
    out.extend(forced[: min(len(forced), n)])
    while len(out) < n:
        if rng.random() < 0.8:
            a = rng.choice(_EDGES)
            b = rng.choice(_EDGES)
        else:
            a = rng.getrandbits(32)
            b = rng.getrandbits(32)
        out.append((a, b))
    return out


def _with_noise(rng: random.Random, body: list[str]) -> list[str]:
    """Sometimes separate a flag producer from its consumer."""
    if rng.random() < 0.65:
        body.insert(-2, rng.choice(_FLAG_PRESERVING_NOISE))
    return body


def _cmp_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, rhs = _WIDTHS[width]
    op = rng.choice(("cmp", "test"))
    cond = rng.choice(_CONDS)
    body = [f"{op} {lhs}, {rhs}", f"set{cond} dl", "movzx eax, dl"]
    body = _with_noise(rng, body)
    return Case(
        f"fuzz_{index:04d}_{op}_set{cond}_i{width}",
        f"fuzz: {op} i{width} with a later SET{cond.upper()}",
        body,
        _inputs(rng),
    )


def _arith_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, rhs = _WIDTHS[width]
    op = rng.choice(("add", "sub", "and", "or", "xor"))
    cond = rng.choice(_NON_CARRY_CONDS)
    body = [f"{op} {lhs}, {rhs}", f"set{cond} dl", "movzx eax, dl"]
    body = _with_noise(rng, body)
    return Case(
        f"fuzz_{index:04d}_{op}_set{cond}_i{width}",
        f"fuzz: {op} i{width} flag result consumed by SET{cond.upper()}",
        body,
        _inputs(rng),
    )


def _incdec_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    op = rng.choice(("inc", "dec"))
    carry = rng.choice(("stc", "clc"))
    consumer = rng.choice(("adc eax, 0", "sbb eax, 0"))
    body = [carry, f"{op} {lhs}", rng.choice(_FLAG_PRESERVING_NOISE), consumer]
    return Case(
        f"fuzz_{index:04d}_{carry}_{op}_{consumer.split()[0]}_i{width}",
        f"fuzz: {op} i{width} must preserve incoming carry",
        body,
        _inputs(rng),
    )


def _neg_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    consumer = rng.choice(("adc eax, 0", "sbb eax, 0"))
    body = [f"neg {lhs}", rng.choice(_FLAG_PRESERVING_NOISE), consumer]
    return Case(
        f"fuzz_{index:04d}_neg_{consumer.split()[0]}_i{width}",
        f"fuzz: NEG i{width} carry survives flag-preserving instructions",
        body,
        _inputs(rng),
    )


def _shift_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    op = rng.choice(("shl", "shr", "sar", "rol", "ror"))
    # Compare the shifted/rotated value directly.  Counts that mask to zero
    # preserve the incoming flags, while ROL/ROR preserve ZF/SF for every
    # count.  Attaching an arbitrary SETcc would therefore compare harness
    # setup flags rather than the instruction under test and create false
    # positives.  Dedicated cases with explicit flag setup can fuzz those
    # semantics separately.
    body = [f"{op} {lhs}, cl"]
    inputs = _inputs(rng)
    counts = (0, 1, max(1, width - 1), width, 31, 32, 33, 255)
    inputs[: len(counts)] = [(_EDGES[i % len(_EDGES)], c) for i, c in enumerate(counts)]
    return Case(
        f"fuzz_{index:04d}_{op}_i{width}",
        f"fuzz: {op} i{width} count masking and result preservation",
        body,
        inputs,
    )


def _zeroing_case(rng: random.Random, index: int):
    width = rng.choice(tuple(_WIDTHS))
    lhs, _ = _WIDTHS[width]
    incoming = rng.choice(("stc", "clc"))
    consumer = rng.choice(("adc eax, 0", "sbb eax, 0"))
    body = [incoming, f"xor {lhs}, {lhs}", rng.choice(_FLAG_PRESERVING_NOISE), consumer]
    return Case(
        f"fuzz_{index:04d}_xorzero_{consumer.split()[0]}_i{width}",
        f"fuzz: XOR self i{width} clears carry before {consumer.split()[0].upper()}",
        body,
        _inputs(rng),
    )


# ---------------------------------------------------------------------------
# Wider families. The originals cover flags from one producer to one SETcc;
# the bugs a real title hit were elsewhere: a sign-extend from bp lifted as a
# zero-extend (Bink's IDCT), signed conditions after an overflowing add. These
# reach the register forms, multiply/divide, carry chains, partial registers
# and multi-instruction flag tracking. eax is compared, so every body leaves
# its answer there. ebp and esp stay out: the native side is a gcc frame.

_BYTE_REGS = ("al", "cl", "dl", "bl", "ah", "ch", "dh", "bh")
_WORD_REGS = ("ax", "cx", "dx", "bx", "si", "di")
_PREFILL = ["mov ebx, ecx", "mov esi, eax", "mov edi, ecx", "xor edi, 0x5A5A5A5A"]
# Conditions that are defined after a multiply (CF/OF only) or a bit test
# (CF only): anything else reads an undefined flag, which the CPU does settle
# but the lifter is free to settle differently.
_CF_CONDS = ("b", "ae")


def _movx_case(rng, index):
    op = rng.choice(("movsx", "movzx"))
    src = rng.choice(_BYTE_REGS + _WORD_REGS)
    dst = rng.choice(("eax", "ecx", "edx", "ebx", "esi", "edi"))
    body = _PREFILL + [f"{op} {dst}, {src}"]
    if dst != "eax":
        body.append(f"mov eax, {dst}")
    return Case(f"fuzz_{index:04d}_{op}_{dst}_{src}",
                f"fuzz: {op} {dst}, {src} (every register form)", body, _inputs(rng))


def _mul_case(rng, index):
    form = rng.choice(("imul eax, ecx", "imul ecx", "imul cx", "imul cl", "mul ecx",
                       "mul cx", "mul cl", "imul3"))
    if form == "imul3":
        imm = rng.choice((-128, -7, -1, 3, 127, 2896, -5352, 0x12345))
        form = f"imul eax, ecx, {imm}"
    want = rng.choice(("lo", "hi", "cf"))
    body = [form]
    if want == "hi":
        body.append("mov eax, edx")
    elif want == "cf":
        body += [f"set{rng.choice(_CF_CONDS)} dl", "movzx eax, dl"]
    return Case(f"fuzz_{index:04d}_mul_{want}", f"fuzz: {form}, {want}", body, _inputs(rng))


def _carry_chain_case(rng, index):
    width = rng.choice(tuple(_WIDTHS))
    a, b = _WIDTHS[width]
    first = rng.choice(("add", "sub", "cmp", "neg", "stc", "clc"))
    body = [f"{first} {a}, {b}" if first in ("add", "sub", "cmp") else
            (f"neg {a}" if first == "neg" else first)]
    for _ in range(rng.randint(1, 3)):
        body.append(f"{rng.choice(('adc', 'sbb'))} {a}, {b}")
    if rng.random() < 0.5:
        body += [f"set{rng.choice(('b', 'ae', 'z', 'nz', 's'))} dl", "movzx eax, dl"]
    return Case(f"fuzz_{index:04d}_carry_chain_i{width}",
                f"fuzz: {first} then adc/sbb chain, i{width}", body, _inputs(rng))


_PARTIAL_OPS = ("mov ah, cl", "mov al, ch", "add al, cl", "sub ah, ch", "xor ah, al",
                "and al, ch", "or ah, cl", "inc ah", "dec al", "neg ah", "not al",
                "mov ax, cx", "add ax, cx", "xchg al, ah", "mov dh, al", "add dl, ah",
                "shl ah, 1", "sar al, 1", "rol ax, 3", "movzx eax, ax", "movsx eax, ah",
                "mov dx, ax", "sub dh, cl", "adc al, cl", "sbb ah, ch")


def _partial_case(rng, index):
    body = rng.sample(_PARTIAL_OPS, rng.randint(2, 5))
    if rng.random() < 0.5:
        body.append("mov eax, edx" if rng.random() < 0.3 else "add eax, edx")
    return Case(f"fuzz_{index:04d}_partial", "fuzz: partial-register writes and reads",
                body, _inputs(rng))


def _bt_case(rng, index):
    op = rng.choice(("bt", "bts", "btr", "btc"))
    src = rng.choice(("ecx", str(rng.choice((0, 1, 7, 15, 16, 31)))))
    want = rng.choice(("cf", "val"))
    body = [f"{op} eax, {src}"]
    if want == "cf":
        body += [f"set{rng.choice(_CF_CONDS)} dl", "movzx eax, dl"]
    return Case(f"fuzz_{index:04d}_{op}_{want}", f"fuzz: {op} eax, {src}", body, _inputs(rng))


def _cmov_case(rng, index):
    width = rng.choice(tuple(_WIDTHS))
    a, b = _WIDTHS[width]
    op = rng.choice(("cmp", "test", "sub", "add", "and", "xor"))
    conds = _CONDS if op in ("cmp", "test") else _NON_CARRY_CONDS + ("b", "ae")
    cond = rng.choice(conds)
    body = ["mov edx, eax", f"{op} {a}, {b}", f"cmov{cond} edx, ecx", "mov eax, edx"]
    return Case(f"fuzz_{index:04d}_{op}_cmov{cond}_i{width}",
                f"fuzz: {op} i{width} then CMOV{cond.upper()}", body, _inputs(rng))


def _shift_flags_case(rng, index):
    width = rng.choice(tuple(_WIDTHS))
    a, _ = _WIDTHS[width]
    op = rng.choice(("shl", "shr", "sar"))
    cnt = rng.choice([c for c in (1, 2, 3, 5, 7, 8, 15, 16, 31) if c < width] or [1])
    cond = rng.choice(("b", "ae", "z", "nz", "s", "ns"))
    body = [f"{op} {a}, {cnt}", f"set{cond} dl", "movzx eax, dl"]
    return Case(f"fuzz_{index:04d}_{op}{cnt}_set{cond}_i{width}",
                f"fuzz: flags after {op} i{width} by {cnt}", body, _inputs(rng))


def _ext_case(rng, index):
    body = [rng.choice(("cwde", "cbw", "cdq", "cwd", "bswap eax", "xchg al, ah",
                        "xchg eax, ecx", "xadd eax, ecx", "not eax", "neg ax"))]
    if body[0] in ("cdq", "cwd") and rng.random() < 0.5:
        body.append("mov eax, edx")
    return Case(f"fuzz_{index:04d}_{body[0].split()[0]}", f"fuzz: {body[0]}", body,
                _inputs(rng))


_CHAIN_OPS = ("add eax, ecx", "sub eax, ecx", "and eax, ecx", "or eax, ecx",
              "xor eax, ecx", "add ecx, eax", "sub ecx, eax", "imul eax, ecx",
              "shl eax, 3", "shr eax, 5", "sar eax, 7", "neg eax", "not ecx", "inc eax",
              "dec ecx", "lea eax, [eax+ecx*2+5]", "lea ecx, [ecx*8-3]", "mov edx, eax",
              "add edx, ecx", "sub eax, edx", "xor ecx, edx", "sar ecx, 31",
              "and eax, 0xFFFF00FF", "add al, cl", "sub cx, ax")
_CHAIN_SETTERS = ("add eax, ecx", "sub eax, ecx", "cmp eax, ecx", "test eax, ecx",
                  "and eax, ecx", "xor ecx, eax", "add ecx, edx", "sub edx, eax",
                  "cmp cx, ax", "sub al, cl", "inc eax", "dec ecx", "neg eax")


def _chain_case(rng, index):
    body = [rng.choice(_CHAIN_OPS) for _ in range(rng.randint(2, 6))]
    setter = rng.choice(_CHAIN_SETTERS)
    m = setter.split()[0]
    if m in ("inc", "dec"):
        conds = tuple(c for c in _CONDS if c not in ("b", "ae", "be", "a"))
    elif m in ("and", "test", "xor"):
        conds = _CONDS
    else:
        conds = _CONDS
    cond = rng.choice(conds)
    body += [setter, f"set{cond} dl", "movzx eax, dl"]
    if rng.random() < 0.4:
        body.insert(-2, rng.choice(_FLAG_PRESERVING_NOISE))
    return Case(f"fuzz_{index:04d}_chain_{m}_set{cond}",
                f"fuzz: random chain then {setter} feeding SET{cond.upper()}", body,
                _inputs(rng))


_FAMILIES = (_cmp_case, _arith_case, _incdec_case, _neg_case, _shift_case, _zeroing_case,
             _movx_case, _mul_case, _carry_chain_case, _partial_case, _bt_case,
             _cmov_case, _shift_flags_case, _ext_case, _chain_case)


def generate_cases(count: int, seed: int) -> list[dict]:
    """Generate ``count`` deterministic conformance Case dictionaries."""
    if count < 1:
        raise ValueError("count must be >= 1")
    rng = random.Random(seed)
    cases = []
    for i in range(count):
        family = rng.choice(_FAMILIES)
        cases.append(family(rng, i))
    return cases


def _case_listing(cases: Iterable[dict]) -> str:
    lines = []
    for case in cases:
        lines.append(f"{case['name']}: " + "; ".join(case["asm"]))
    return "\n".join(lines)


def _format_seed(seed: int) -> str:
    """Return a seed spelling argparse can parse again verbatim."""
    return str(seed) if seed < 0 else f"0x{seed:X}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="python -m tools.conformance.fuzz",
        description="Generate deterministic x86 snippets and compare native execution to the lifter.",
    )
    ap.add_argument("--count", type=int, default=200, help="number of generated snippets (default 200)")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x58424F58,
                    help="PRNG seed, decimal or 0x-prefixed (default 0x58424F58)")
    ap.add_argument("--list", action="store_true", help="print generated snippets without compiling them")
    ap.add_argument("--keep", action="store_true", help="keep conformance build artifacts")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        cases = generate_cases(args.count, args.seed)
    except ValueError as exc:
        ap.error(str(exc))

    seed_text = _format_seed(args.seed)
    print(f"xboxrecomp lifter fuzz seed={seed_text} count={len(cases)}")
    if args.list:
        print(_case_listing(cases))
        return 0

    # Reuse the production differential runner rather than maintaining a
    # second assembler/lifter/harness path.  Only the case corpus is replaced.
    from . import __main__ as runner

    runner.CASES = cases
    runner._WHY = {c["name"]: c["why"] for c in cases}
    runner._TOL = {c["name"]: c.get("tol", 0.0) for c in cases}

    runner_args = ["--only", "snippets"]
    if args.keep:
        runner_args.append("--keep")
    if args.verbose:
        runner_args.append("--verbose")
    rc = runner.main_with_args(runner_args)
    if rc:
        print(
            f"\nReproduce with: py -3 -m tools.conformance.fuzz --count {args.count} "
            f"--seed {seed_text} --keep -v",
            file=sys.stderr,
        )
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
