# Orbis Runtime Foundation

This change prepares **guest data images**, without running a game. It reuses
the existing linker and native loaders; no PFS, DRM, encrypted SELF, game file
or added Sony NID is involved in the fixtures.

## Audit of main (08db54e)

| Existing component | Function already implemented | Blocker observed and decision |
| --- | --- | --- |
| `PS4ExecutableImage` | Orbis x86-64 ELF64 and plaintext SELF, PT_LOAD/SCE_RELRO, BSS, logical permissions, bounds, 32 MiB/module | Reserved ELF fields, section declarations and unmapped metadata ranges needed extra checks. Hardened in this loader. The linker still refuses any SELF. |
| `PS4PKGRuntimePlan` | PKG inventory, standalone staging, explicit closed graph, path cycles/ambiguities, 128 modules/64 MiB | The inventory does not provide executables from an opaque PFS. The existing graph is reused; no change to the PKG parser. |
| `OrbisMetadata` / `OrbisPRXPreparation` | SCE inventory, HLE recognition, experimental PRX staging, RELA and diagnostics | A separate analysis path, not a runtime binding. Kept for compatibility; the new report uses the existing transactional linker, without a new loader. |
| `OrbisLinker` | Typed NID identities, exact exports, RELA 1/6/7/8, deterministic graph, strong transaction, W^X and RELRO | Required a static ELF to have SCE tables; unused dependencies were not checked; DT_NEEDED refused; rebased addresses checked for overflow but not canonicality. Fixed here. |
| `OrbisLinkerBridge` | Owning handle, guest mapping, entry and data copy, no host pointer in the ABI | Diagnostics limited to the first error. A preparation report API is added, compatible with the existing calls. |
| `PS4VirtualMemory`, libkernel and threads | Private 16 KiB anonymous mappings, protections, unmapping, clocks, bounded files/mutexes | Attached to the existing GuestELF path; no automatic connection to multi-module SCE images. Their regressions still run. |
| `PS4DynamicLinker` / `maxps4_fex_runtime` | Bounded synthetic path with standard DT tables, libkernel trampolines, instruction admission and FEX | Does not support SCE tables/general PRX graphs. FEX receives rebased host addresses for the admitted subset, not an arbitrary Orbis address-space backend. No handoff of a vector pointer to the FEX RIP. |
| iOS diagnostics view / `OrbisAnalysisBridge` | Picking a file and sharing the report through Swift/C | The inventory report did not prove a runtime preparation. It now includes a real, temporary native attempt, with no external provider and no fake HLE registry. |

## Added behavior

An `ET_SCE_EXEC` with neither PT_DYNAMIC nor DYNLIBDATA can be prepared alone,
with several segments and BSS. A dynamic executable or PRX still requires the
SCE STRTAB/SYMTAB tables and the explicitly implemented formats. No file name
grants ELF admission. Unknown runtime headers (notably PT_INTERP and
uninterpreted SCE parameters) are refused; the accepted headers are NULL, LOAD,
DYNAMIC, NOTE, PHDR, TLS (refused later), DYNLIBDATA, SCE_RELRO. No implicit
support for Linux ELFs or protected SELFs.

The loader checks the reserved e_ident bytes, e_flags and section-table
consistency. Without sections, e_shentsize 0 or 64 is accepted; extended
numbering is not implemented. The data of each file-backed ELF header stays in
the file, even for unmapped metadata.

The linker validates request limits **before** copying/sorting them, and file
sizes before reading headers. It refuses any rebased segment whose end exceeds
`2^47`, on top of the existing overflow and alignment checks. Guest regions do
not overlap. ELF permissions R=4/W=2/X=1 stay distinct from PS4 protections
PROT_READ=1/WRITE=2/EXEC=4.

Each `DT_SCE_NEEDED_MODULE` and `DT_SCE_IMPORT_LIB` descriptor must find an exact
provider among the explicitly provided dependencies: name and version, even
with no symbol using that descriptor. The diagnostic distinguishes a missing
provider, an incompatible version, an undeclared dependency and an ambiguity.
`DT_NEEDED` is a bounded offset in the SCE string table; its value must name an
exact path declared in the graph. Empty/duplicate/invalid values are refused.
There is no implicit disk search or suffix resolution.

Referenced or unused imports must still find a complete compatible export
(NID/library/module/versions/type). Missing weak imports remain refused. The four
existing RELA formulas and their checks are kept. No unknown relocation is
silently deferred. FUNC exports must belong entirely to file-backed RX code: a
function located in BSS is not enough to resolve an import.

Since then, two loader cases found in real games were added: an undefined
`_GLOBAL_OFFSET_TABLE_` resolves to the module's own SCE PLTGOT, and the symbol
index of an R_X86_64_RELATIVE relocation is ignored (Call of Duty: Modern
Warfare Remastered's `libc.prx`).

## Diagnostics and transaction

`OrbisLinker::Report`, an optional argument of `link`, describes the stage
reached, the validated ELFs/segments, the checked dependencies, the
examined/resolved imports, the examined/validated/applied/deferred/refused
relocations and the memory. On error, the counters are **partial** and do not
claim to describe the following stages; zero relocations are reported as
applied and no new image is committed. Validated writes cancelled with the
transaction are counted as deferred, with no way to resume the rejected image.
The first explicit cause is kept. Subset limits are a preparation refusal, not
an execution success.

`maxps4_orbis_link_with_report` exposes this preparation to the C/Swift bridge,
with the same inputs and the same handle as `maxps4_orbis_link`. Provide a
4096-byte buffer. If the success report does not fit, `-ENOSPC` is returned and
the handle stays null. Refusal reports may be truncated to the capacity and stay
terminated. Callers serialize calls and keep the input buffers immutable during
the call.

The existing "Orbis analysis" view runs a preparation of the selected file with
zero dependencies provided: a missing external PRX stays missing. The
inventory's HLE recognition is explicitly distinguished from runtime bindings.
The temporary image is destroyed after the diagnostic; it is not kept for
execution. The report still shows that the general FEX Orbis backend, the
stack/TLS and the ABI are missing. The multi-module bridge exists for a future
module picker; the current iOS selection covers a single file.

`Image::mappings()` borrows the image's owned bytes: they stay valid when that
image is moved and become invalid when it is replaced/destroyed. The backend
must copy the bytes or keep their owner. The C bridge does not expose these
pointers. Segments remain host data vectors, without PROT_EXEC. RELRO goes from
RW to R after relocations. Protections are logical; the regions are not yet FEX
page mappings. Logical segments may share a future host page: a backend will
have to decide on/refuse their page constraints, without silently widening
rights.

## Reproducible tests

```sh
bash scripts/test-orbis-runtime.sh
bash scripts/test-orbis-analysis.sh
bash scripts/test-ps4-system-services.sh
```

The new script builds as C++17 with fatal warnings, ASan/UBSan and runs the
tests under a bounded timeout. The fixtures stay synthetic. The PKG workflow
runs the same script on Linux and macOS, then the existing Swift/C tests and
iOS ARM64 build. The SideStore IPA workflow keeps its linker test, the two
native bridges and its packaging; no change to the PKG inventory path.

The regressions include a multi-segment static ELF, BSS/permissions and exact
limits, truncation at each of the file's 3072 lengths, reserved headers and
inconsistent sections, an invalid metadata range, non-canonical rebased
addresses, request limits, an unused dependency, three modules/two
dependencies, valid/duplicate/out-of-bounds DT_NEEDED, incompatible
module/version/library, ambiguous identity, refusal of unknown runtime headers
and a late failure report. The existing checks for a missing NID, ambiguous
exports, four relocations, cycles, RELRO, overflows, an intact previous
transaction, the HLE registry and the ABI stay active. The Swift reports are
checked in the macOS/iOS workflows.

Local observations and checks that could not be run are kept separately in
[orbis-runtime-validation.md](orbis-runtime-validation.md).

## Functions still needed before a game (at the time of this PR)

| Function | State after this PR | Work needed |
| --- | --- | --- |
| Plaintext SCE ELF/PRX | Bounded, deterministic, transactional preparation | Extend only with documented formats and fixtures |
| Dependencies/NIDs | Exact check of the provided graph and compatible exports | Real legal providers; verified HLE catalog, no invented symbols |
| Relocations | x86-64 RELA 64/GLOB_DAT/JUMP_SLOT/RELATIVE | TLS, PC-relative, other tags/types as needed; unknown ones refused |
| Memory/FEX | Logical mapping contract and C copy; W^X/RELRO | Multi-module FEX backend, page policy, lifetime, memory fault/signal |
| Process/threads | Existing bounded synthetic services | Process stack, arguments, per-thread TLS, Orbis schedulers/ABI |
| PRX life cycle | Dependency order prepared, cycles refused | SCE parameters, init/fini, coordinated unload; no function called |
| x86-64 to ARM64 | Existing bounded synthetic test, separate from general SCE images | Connect the general mappings to the real FEX, then test on ARM64/iPhone |
| iPhone diagnostics | Analysis + temporary native preparation of one file; multi-module bridge | Multi-module picker and durable image management |
| GPU/audio/input | Existing bounded prototypes | ABI, synchronization and full pipeline for a game |
| Protected data | Opaque PFS/DRM, unchanged | No decryption or circumvention in this mission |

A successful preparation of the fixtures is not real PS4 execution.
