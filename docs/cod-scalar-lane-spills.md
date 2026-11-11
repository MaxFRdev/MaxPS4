# Call of Duty: scalar spills in VGPR lane slots

The device report dated 2026-11-11 00:45:30 stops at V_WRITELANE_B32.
The captured program saves s76..s79 into lanes 4..7 of v53, then restores
them into s16..s19 before reusing v53 for ordinary vector arithmetic.
The complete captured shader translates offline with this change. Its words
and generated source remain in ignored build files.

VOP2 lane selectors are scalar operands, not VGPR numbers. READLANE writes
an SGPR. Decoder diagnostics now reflect both facts for VOP2 and VOP3.
The interpreter models actual 64-lane reads and writes, masks the lane index
to six bits and executes both instructions independently of EXEC.

The translator accepts a narrower pattern: scalar values saved in constant
lane slots and read back within one basic block. Every read requires an
earlier store. Ordinary vector reads while the bank contains spilled data,
unproved overwrites, relative register access, branches across the lifetime,
and EXEC changes before the last bank use are refused. A definite ordinary
vector overwrite ends the spill lifetime and invalidates initialized slots.
Slots become private scalar variables in the existing per-invocation SGPR
execution model. This adds no general wave64 exchange or native SIMD shuffle.
That existing SGPR/branch model still limits fidelity for divergent waves.

Tests compare translated C++ with the independent 64-lane interpreter,
including EXEC=0, lane 63, scalar clobbers and ordinary bank reuse. Separate
interpreter checks cover every lane and dynamic masked indices in both
encodings. Negative translation tests cover unsafe lifetimes and selectors.
The Metal test checks four sets of four scalar values at every target pixel.
Full game rendering and performance still require a physical-device test.

Resource discovery normally interprets only lane 0. It may now temporarily
expand to 64-lane storage when a lane exchange needs another slot, resuming
at that instruction with the current SGPRs, EXEC and LDS rather than replaying
earlier memory loads. Unobserved lanes start at zero; this remains a
representative scan, not discovery of every divergent resource path. The
single-lane storage is restored afterward, and normal interpretation retains
its explicit lane bounds. A repeated scanner regression checks a constant
load after four spills including lane 63, and verifies no duplicate loads.
