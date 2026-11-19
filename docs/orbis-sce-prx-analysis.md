# Orbis ELF analysis and incremental PRX preparation

Non-regression reference: `bd30c050c65243af577498ef04d34aa1faced1d8`, PR #171.
The existing `GuestELF → PS4DynamicLinker → FEXCore → libkernel` path is not
widened by this delivery. The iPhone diagnostics still expect 42 for the
FEXCore x86-64 JIT and 2 for the ELF/libkernel test, with backend 2 and cleanup.
Their success on the new IPA must be confirmed on a physical iPhone.

## What is delivered

In **FEXCore diagnostics**, **Analyze an Orbis executable** opens the app's file
picker. Pick an unencrypted ELF/PRX reachable through Files. The file is read on
the diagnostics queue, with security-scoped access kept until reading ends. The
report is selectable and shareable. This action creates no FEX engine and no
guest/JIT region, and does not run the file. The demo run button remains a
separate action.

`OrbisMetadata` inspects little-endian x86-64 ELF64, System V/FreeBSD OSABI,
ET_EXEC/ET_DYN and the SCE types EXEC, REPLAY_EXEC, STUBLIB, DYNEXEC, DYNAMIC.
Replay/stub files are identified but not prepared. Section tables may be absent;
the inventory relies on PT_DYNAMIC and PT_SCE_DYNLIBDATA. PT_LOAD, TLS, RELRO,
process/module parameters and other known segments are named and shown with
their addresses, offsets, sizes, alignments and flags. Process/module
parameters, comments and segment versions remain opaque metadata; no startup
contract is inferred from their presence.

DT_SCE_STRTAB/SYMTAB/RELA/JMPREL/HASH offsets are relative to the start of
PT_SCE_DYNLIBDATA. Standard DT_* entries use virtual addresses translated only
within bytes of loadable segments. Standard symbol tables require DT_HASH;
GNU_HASH alone is not supported yet. SCE SYMTABSZ gives the symbol count.
Strings are bounded and terminated, tables are checked before reading and
contradictory fields are refused. Module versions are minor in bits 32..39 and
major in 40..47; libraries use a 16-bit version. Identifiers are in bits
48..63. `NID#library#module` suffixes are decoded with the SCE alphabet
`ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-`.

Each import/export is matched to its declared identities and versions, not to
guessed library names. The existing libkernel resolver is queried only for
qualified imports, library version 1 and module 1.0. "Resolved HLE (analysis)"
means the service exists in the table; no trampoline is installed and no module
execution is validated. An unknown NID, another library/version or a missing
identity does not produce a simulated success. The report counts resolved and
unresolved imports.

## Limited PRX preparation

`OrbisPRXPreparation` is a tested C++ API, separate from the runtime and the
single-file picker. It explicitly receives 1..32 unencrypted ET_SCE_DYNAMIC
files and their load biases. It copies PT_LOAD/PT_SCE_RELRO and BSS into
ordinary byte vectors, keeps the declared protections and refuses W+X. It
creates no executable host memory. No change to the StikDebug RX/RW provider,
to the protections of FEX mappings or to the linker validated on iPhone.

SCE dependencies are looked up by exact module name/version among the files
provided; DT_NEEDED matches the explicit name of a provided file. No search on
disk, in a PKG or through an RPATH. Cycles, ambiguous modules, missing
dependencies and ambiguous exports are refused. Visible global exports resolve
the qualified imports of direct dependencies with identical
NID/library/module and versions. No interposition, weak/GNU symbols,
special/ABS symbols or PRX HLE trampoline in this first API.

The relocations applied are R_X86_64_NONE, 64, GLOB_DAT, JUMP_SLOT and RELATIVE.
GLOB_DAT/JUMP_SLOT require a zero addend. RELATIVE is base + addend; a symbol
index on it is ignored, as the ELF x86-64 ABI specifies (Call of Duty: Modern
Warfare Remastered's `libc.prx` sets one). Signed addends and overflows are
checked; targets must hold eight writable bytes, without overlap. PC32, COPY,
TLS and other types remain refused by the preparation, even though the separate
synthetic linker already supports some additional types. Any failure destroys
the partial preparation and clears a previous success. `unload()` frees the
owned buffers; input files stay with the caller. The topological initialization
order and DT_INIT/DT_FINI are kept; no constructor/finalizer is called. There is
no integrated PRX execution cycle.

## Memory, TLS and limits

The PRX bias must be 16 KiB aligned and respect every p_align. Written values
are logical guest addresses, never ARM64 pointers. The preparation accepts only
the canonical lower half of the x86-64 address space; it does not claim to
implement the Orbis address space, ASLR or startup ABIs. PT_TLS must be
contained in a readable PT_LOAD, with a consistent file image. The report gives
the initial size, total size and alignment. No TLS module ID, DTV, FS base,
per-thread block or TLS relocation is provided yet; the preparation refuses
these needs. RELRO regions are not finalized in a runtime.

Budgets: 64 MiB per analyzed file, 128 segments, 4096 symbols and relocations,
1 MiB of STRTAB, printable ASCII strings of 1024 bytes, 2 MiB report
(truncation reported). The PRX preparation also enforces 64 MiB in total for
the files, then 64 MiB for the segment buffers. A real file exceeding these
limits is explicitly refused. SELF is refused, with no attempt at decryption,
DRM, license, key or extraction of a commercial file. Unknown tags and opaque
SCE attributes are reported as obstacles, not applied.

This delivery establishes no Kero Blaster compatibility and no game startup.

## Validation and iPhone acceptance

`bash scripts/test-orbis-analysis.sh` builds the fully synthetic fixtures under
ASan/UBSan and caps the test at 30 seconds. It exports a small fixture
`build/orbis/synthetic-sce.prx` for the Swift test and the Files acceptance
test. The tests cover SCE and standard tables, qualified exports/imports,
versions, signed relocations, two dependent PRXs, cycles, minimal inventoried
TLS, corruption, invalid addresses/sizes/alignments, report truncation and
cleanup. `bash scripts/test-ps4-system-services.sh` keeps the 12 existing native
suites and now caps each executable at 30 seconds.

The `Orbis SCE analysis and PRX preparation` CI tests the Linux/macOS natives,
the real Swift bridge, and builds C++/Swift for iphoneos ARM64. The existing IPA
CI keeps its FEX, libkernel, memory/JIT, Metal, Swift, audit and leak tests. The
local sandbox prevents LeakSanitizer under ptrace: the local check uses
`ASAN_OPTIONS=detect_leaks=0`; CI does not disable this check. No Swift, ARM64
or IPA result is inferred from a native Linux test.

On iPhone after installing the IPA build 1000172 (or a later fix build):

1. Import the synthetic fixture from Files through **Analyze an Orbis executable**.
   Check ET_SCE_DYNAMIC/PRX, x86-64, segments, libkernel 1.0/library v1,
   NID aI+OeCz8xrQ resolved, relocations, obstacles and no execution.
2. Analyze an unencrypted ELF/PRX with known metadata and compare the report
   with readelf and the SCE declarations. Required modules remain absent from
   the single-file analysis; they are not loaded automatically.
3. With StikDebug authorized, rerun **Test FEXCore**: result 42, real ARM64 JIT
   backend. Then **Test PS4 ELF / libkernel**: 2 PT_LOAD, scePthreadSelf import,
   libkernel call/return 1, guest resume, final value 2, backend 2 and zero
   remaining guest regions. Share the reports and the iOS model/version.

Constants/layouts were cross-checked with shadPS4 `elf.h`, `module.h` and
`module.cpp` at `443f022450f6857256e780daa3d5a14029a748f7` (GPL-2.0-or-later).
