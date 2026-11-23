# Call of Duty: M0-relative VGPR source reads

The device report dated 2026-11-11 01:23:57 stops in pixel program
`0x70847c4700` at `v_movrels_b32`. The captured program sets `s32` to zero,
loops while `float(s32) < s33`, computes `M0 = 3 * s32`, then reads:

```
v22 = VGPR[4 + M0]
v21 = VGPR[5 + M0]
v20 = VGPR[6 + M0]
```

It restores M0 and increments `s32` afterward. Explicit cases for indices
one, two and three show the intended four float3 values in v4..v15, but the
loop bound comes from a buffer and cannot safely be assumed to be four.

`SPI_SHADER_PGM_RSRC1_PS.VGPRS` supplies the exact hardware allocation:
`4 * ((RSRC1 & 0x3f) + 1)`, from 4 through 256 registers. The draw collector
now carries both VS and PS allocations, and compute dispatches carry their
allocation from `COMPUTE_PGM_RSRC1`. The translator receives that bound and
emits an M0 switch from the encoded source register to the last allocated
VGPR. A source outside the allocation reads v0, matching Southern Islands.
Relative destination instructions remain refused.

The allocations are part of render and compute pipeline cache keys because
the same program address with another RSRC1 value needs different generated
source. The resource scanner and software renderer also give the allocation
to the interpreter. V_MOVRELS applies EXEC per lane; the interpreter tests
partial EXEC, every valid offset in the fixture, and the v0 fallback.

Validation includes VOP1/VOP3 decoding, missing and too-small allocation
refusals, RSRC1 collection for draws and dispatches, scanner continuation,
64-lane interpreter comparison, generated C++ execution, and compilation of
the complete captured shader. Its 1,098 private words and generated source
remain under ignored `build/toolchain/test-results` files.

The generated switch may increase Metal compilation time and register
pressure for shaders with a large allocation. Full rendering and performance
of this game shader still require the macOS Metal checks and an iPhone test.
