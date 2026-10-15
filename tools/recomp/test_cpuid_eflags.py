"""cpuid and pushfd/popfd lift to real operations, not RECOMP_UNIMPL no-ops.

As no-ops, cpuid left the feature registers holding garbage (RAD's MMX probe
read "no MMX" and Bink took a fallback path no console runs) and pushfd;
pop eax popped the caller's data off the stack."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000


def lift(hexbytes):
    image = bytes.fromhex(hexbytes)
    config._install([config.Section('.text', BASE, 0x1000, 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='cpuid-test')
    db = {BASE: {'end': BASE + len(image), 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_cpuid_calls_the_runtime():
    code = lift('b801000000 0fa2 c3')          # mov eax,1; cpuid; ret
    assert 'xbox_Cpuid(&eax, &ebx, &ecx, &edx);' in code
    assert 'RECOMP_UNIMPL("cpuid"' not in code


def test_pushfd_popfd_round_trip_the_shadow_word():
    code = lift('9c 58 35 00002000 50 9d c3')  # pushfd; pop eax; xor eax,ID; push eax; popfd
    assert 'PUSH32(esp, g_eflags_shadow);' in code
    assert 'POP32(esp, g_eflags_shadow);' in code
    assert 'RECOMP_UNIMPL' not in code
