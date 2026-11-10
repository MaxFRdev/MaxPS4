# Call of Duty: LDS shader capture

The latest device report stops on DS opcode 13, words `d8340000 00002022`
at pixel shader offset 0x180. This is a local-data-share write, with an address
from v34 and data from v32. The runtime currently has no general LDS model;
only quad swizzles are translated, and the interpreter has the same limitation.

The single instruction does not reveal the subsequent LDS reads, barriers,
address construction, or whether the operation exchanges data between the two
halves of a guest wave64. Replacing it with a no-op or lane-private storage
would silently change the shader. The previous native SIMD mapping does not
provide complete PS4 wave64 shared execution.

Failed vertex and pixel translation now append the refused shader's bounded
raw listing and user SGPRs to the existing report. This reuses the existing
programWords capture (4096 words maximum, readable-code checks, decoding bounds)
and deduplicates by shader address. It bypasses the first-draw listing cap and
retains decodable code after early ENDPGM instructions for branch targets.

This is a diagnostic improvement. It does not implement LDS or resolve the
reported stop. A new device report is required to study the complete shader.

The first diagnostic build still returned the previous immutable report when
submit exited on translation failure. The bridge now publishes a fresh report
on that error path and on compute-barrier failure. A regression submits valid
draws, then a shader containing the exact refused LDS write, and checks the
public bridge report immediately for the shader address, raw words and error.
