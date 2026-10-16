"""movsx from every 16- and 8-bit register sign-extends.

_lift_movsx listed ax, bx, cx, dx, si and di as the 16-bit sources to wrap in
SX16 and left out bp and sp, so `movsx ebp, bp` fell through to the plain
register read -- LO16(ebp), a zero-extend. Bink's IDCT (Alter Echo,
sub_001A6580) holds a row-6 coefficient in bp and widens it exactly that way;
each negative coefficient came out near 65,000 and the block turned to stripes.
movzx always had all eight.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(m, dst, src):
    insn = Instruction(0, 3, m, "", "00")
    insn.operands = [Operand(type="reg", reg=dst), Operand(type="reg", reg=src)]
    return " ".join(Lifter().lift_instruction(insn))


class MovsxRegs(unittest.TestCase):
    def test_every_word_register_sign_extends(self):
        for src in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            with self.subTest(src=src):
                self.assertIn("SX16(", _lift("movsx", "eax", src))

    def test_bp_into_itself(self):
        out = _lift("movsx", "ebp", "bp")
        self.assertIn("SX16(", out)
        self.assertNotRegex(out, r"ebp\s*=\s*LO16\(ebp\)\s*;")

    def test_every_byte_register_sign_extends(self):
        for src in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
            with self.subTest(src=src):
                self.assertIn("SX8(", _lift("movsx", "eax", src))

    def test_movzx_still_zero_extends(self):
        self.assertIn("ZX16(", _lift("movzx", "ebp", "bp"))


if __name__ == "__main__":
    unittest.main()
