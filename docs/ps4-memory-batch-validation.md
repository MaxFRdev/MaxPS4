# Memory and GNM service validation — 2026-10-29

The kernel additions cover virtual reservations, memory-type protection,
direct-memory queries, named direct mappings, batch mappings and selected GNM
debugger/status imports. Existing service token numbers are preserved.

The regression cases check invalid reservation alignment, null fixed addresses,
physical-range overflow, batch completion counts, early termination, protection
changes, import NIDs and GNM ring-size validation. A general-image integration
case reserves three pages and replaces the middle page with a fixed direct
mapping while retaining inaccessible neighbours.

## Local results

Compiled the five native service translation units with Cygwin GCC 14.4.0,
C++17, `-D_GNU_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pedantic -pthread`.
The toolchain is installed under ignored `build/toolchain`, without registry
registration or shortcuts. This host reports 64 KiB pages. The bounded-memory
test fixture now separates its ELF segments according to the host page size.

Of the 24 tests listed by `scripts/test-ps4-system-services.sh`:

- 20 passed, including `PS4SystemMemoryTimeTest`, libkernel/filesystem, threads,
  synchronization, events, VideoOut, UserService, Pad, AudioOut and both GNM tests.
- `PS4DynamicLinkerTest`, `PS4GuestFrameTest` and
  `PS4OrbisCPUPreparationTest` failed during initial image preparation. All three
  failures reproduce with the libkernel and test sources from HEAD, before this
  change. The new general-image integration case therefore remains unexecuted
  locally.
- `GuestELFMemoryTest` could not compile because Cygwin lacks `mincore`.

Both C ABI probes compiled with C11 and warnings treated as errors.
Local logs and exit statuses are in `build/toolchain/test-results`; the full
result inventory is `summary.json`. Local tests ran without ASan/UBSan.
Linux/macOS sanitizer tests, FEX execution and the iPhone SDK build subsequently
passed in [run 40988358263](https://github.com/MaxFRdev/MaxPS4/actions/runs/40988358263)
on source commit `58b569b`. All 24 service tests passed on macOS, including the
general-image integration case that could not run locally under Cygwin.
See the [macOS validation report](macos-compatibility-validation.md).
No physical iPhone or game execution is claimed.

## Bounded test runner

Validation exposed a separate reporting error: a Windows child exit status of
1536 could be truncated by Bash to exit 0. `scripts/run-bounded.py` now normalizes
out-of-range child exit statuses to 1 while preserving ordinary exit codes and
timeout status 124. A real failing linker test confirmed the corrected status.

`python scripts/test-run-bounded.py -v` passed its four Windows-applicable cases:
success and output, ordinary failure, timeout with partial output, and the
Windows status regression. The POSIX signal case is skipped on Windows. CI now
includes this regression suite on Ubuntu and Windows.
