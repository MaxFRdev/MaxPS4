# Call of Duty: masked bit count

The device now stops on `v_mbcnt_hi_u32_b32`, words `d2480022 000100c1`.
The interpreter already handles the instruction; translation did not.

Pixel/compute translation now supports both low and high halves. The result is
the supplied accumulator plus the population count of mask bits strictly below
the lane index, with low bits numbered 0..31 and high bits 32..63. Boundary
cases avoid shifting a 32-bit word by 32. Vertex translation remains refused.

Generated fragment functions request Metal's `thread_index_in_simdgroup` only
when needed. Compute entry points receive it too. This maps to the native SIMD
group: Apple GPUs typically execute 32 lanes, so this does not combine two
native groups into one virtual PS4 wave64 or provide complete wave64 execution
and cross-lane mask semantics. Existing per-lane EXEC/VCC approximations remain.

Tests compare a low/high chain against the existing 64-lane interpreter,
exhaust all lane positions for several masks and nonzero accumulators, and
translate the exact device instruction. A Metal render compares the chain
against a separate fragment function that returns the native lane index.

Reference: [AMD masked bit count semantics](https://rocm-handbook.amd.com/projects/amd-rocm-optimization-guide/en/docs-1.0.0/compiler-builtins/cross-arch/wavefront-builtins.html).

Actual Call of Duty scene rendering remains to be checked on the iPhone.
