#!/usr/bin/env python3
"""Report the PS4's CPU features to PS4 games, not more.

The PS4's Jaguar cores have AVX, F16C and BMI1 but no AVX2, FMA3, BMI2, VAES
or VPCLMULQDQ. The pinned FEX reports all of them whenever it emulates AVX.
Libraries in PS4 games that pick code by CPUID (Mortal Kombat: Legacy
Kollection's libvpx: "vpx_codec_decode() returned [1]" on its first video)
then run paths the PS4 never ran, through FEX's 128-bit AVX emulation on the
iPhone. Clearing those CPUID bits keeps them on the PS4's paths; instructions
are still decoded and emulated as before.

All edits are validated before writing; repeated application must match exactly.
"""
from pathlib import Path
import sys

TAG = ' // MaxPS4: no PS4 (Jaguar) feature'
NAME = 'FEXCore/Source/Interface/Core/CPUID.cpp'
EDITS = (
    ('            (SupportsAVX() << 12) |                         // FMA3',
     '            (0 << 12) |                                     // FMA3' + TAG),
    ('              (SupportsAVX() << 5) |                   // AVX2 support',
     '              (0 << 5) |                               // AVX2 support' + TAG),
    ('              (SupportsAVX() << 8) |                   // BMI2',
     '              (0 << 8) |                               // BMI2' + TAG),
    ('              (CTX->HostFeatures.SupportsAES256 << 9) | // VAES',
     '              (0 << 9) |                                // VAES' + TAG),
    ('              (SupportsVPCLMULQDQ << 10) |              // VPCLMULQDQ',
     '              ((SupportsVPCLMULQDQ & 0) << 10) |        // VPCLMULQDQ' + TAG),
)


def adapt(root):
    path = root / NAME
    text = path.read_text()
    for old, new in EDITS:
        if text.count(new) == 1:
            continue
        if text.count(old) != 1:
            raise ValueError(f'Pinned CPUID context diverged: {old.strip()}')
        text = text.replace(old, new, 1)
    path.write_text(text)


if __name__ == '__main__':
    adapt(Path(sys.argv[1]))
