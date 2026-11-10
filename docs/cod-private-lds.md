# Call of Duty: proved lane-private LDS scratch

The captured shader has two DS_WRITE_B32 stores, at offsets 0 and 256,
and two DS_READ_B32 reads of the same slots. Every address is rebuilt as
`mbcnt_hi(-1,0)`, `mbcnt_lo(-1,previous)`, then a left shift by two. For
lanes 0..63 these are distinct aligned addresses 0..252. Adding 256 gives
a separate slot for every lane. No other LDS operation occurs in this shader.

The translator lowers this restricted pattern to private variables.
This is not general LDS support and does not rely on combining native SIMD
groups into a guest wave64. The proof checks all reachable DS operations,
the nearest VGPR definitions within the basic block, the exact lane-address
sequence, no intervening EXEC rewrite, supported offsets, and an explicit
M0 limit of 65536. Up to eight 256-byte-spaced slots are recognized. An entry
block store may initialize later blocks; otherwise each read needs an earlier
store in that same basic block. GDS, other strides/offsets, unproved addresses,
different limits and reads without those dominating stores remain refused.

The interpreter independently models DS_READ_B32/DS_WRITE_B32 in bounded,
aligned, per-wave LDS allocated on demand (up to 64 KiB). It does not model
multi-wave groups or LDS barriers. This also lets resource scans continue
past these instructions instead of missing later texture descriptors.

Validation includes 64 distinct lane values, two slots across a branch,
rebuilt addresses, interpreter comparison and negative proof/bounds cases.
A Metal image test stores and restores position-derived values and checks
every pixel. The supplied shader is translated completely offline and its
generated C++ compiles; its private words remain in ignored build files.

Device rendering and performance of the complete shader remain unverified.

The next captured shader uses three slots (0, 256, 512), all stored and read
in a block reached after a branch join. The proof now accepts same-block
initialization there. A branch that can bypass the store remains refused.
DS_READ_B32 and quad swizzle writes are tracked as one VGPR, not four; tests
also reject a read that overwrites the address needed by a later access.

Revision `e02b1d424f566401501769aff82301c58cc90053` passed the
[decoder, interpreter comparison and Metal image tests](https://github.com/MaxFRdev/MaxPS4/actions/runs/42895227215).
The targeted GPU test checked both slots across a branch at every pixel.
The [macOS runtime tests and full IPA build](https://github.com/MaxFRdev/MaxPS4/actions/runs/42895238221)
passed too. Independent non-gating shader-cost variants again reported shared-CI
GPU timeouts. The downloaded IPA's embedded revision, ZIP integrity and SHA-256
were verified before copying `MaxPS4-e02b1d4.ipa` to the desktop.
