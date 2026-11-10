# CUSA05379 literal save strings workaround

Two independently supplied save files reproduce unresolved script-string cache
references during resume. Their body sizes and Adler-32 checksums match the
headers. In segment 8, the observed reader positions resolve to string IDs 183
and 296. Even counting every possible type-2 marker (including false positives)
before those positions gives fewer definitions than those IDs require.
The underlying cause of cache inconsistency is not established.

For the exact observed executable build, use the writer's existing type-3
literal path for every nonempty string instead of its type-2/cache-reference
path. The writer already chooses this path for strings outside its intern pool;
the existing reader explicitly handles type 3. Null and empty encodings, the
reader, outer compression, file layout and file I/O are unchanged.
This is a compatibility workaround, not a repair of existing saves.

## Implementation

- `orbisUseLiteralSaveStrings` verifies fingerprints of both complete writer
  and reader functions before replacing one two-byte branch's opcode. The
  displacement and instruction width stay unchanged.
- The general-image loader applies it to the root executable's private copy,
  alongside the existing fixed-layout workaround. Files on disk are untouched.
- Short, unrelated, changed-reader and already-patched images are left unchanged.
- Fingerprints identify a build; they do not authenticate arbitrary code.

No game executable, disassembly or save data is included in this change.
Literal strings may make new saves larger. Existing invalid cache references
are not reconstructed. Preserve old saves; test by creating a new save from a
new game, then quitting and resuming that new save.

## Validation

Local Cygwin GCC 14.4, C++17 with warnings treated as errors:

- OrbisLinkerTest: 3,515 checks pass, including transactional patching, bounds,
  reader/writer mismatch, unchanged branch width and exactly one changed byte.
- A private local test on the supplied plaintext executable confirms matching
  fingerprints, the literal-path branch destination and refusal after a reader
  change. The original files remain unchanged.

macOS sanitizers, iPhone compilation and device resume validation are pending.
No claim of restored gameplay or recovery of the old saves is made.
