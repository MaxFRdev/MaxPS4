#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for test in PS4LibkernelTest PS4WritableFilesTest PS4SystemMemoryTimeTest PS4ThreadsTest PS4SyncTest PS4EventQueueTest PS4GraphicsEventTest PS4VideoOutTest PS4UserServiceTest PS4PadTest PS4AudioOutTest PS4GnmTest PS4GnmQueueTest PS4DynamicLinkerTest \
  PS4GuestFrameTest PS4OrbisCPUPreparationTest GuestMemoryRangeTest GuestELFInterpreterTest GuestELFMemoryTest PS4GuestMemoryTest \
  PS4GuestStagingTest PS4StagedCPUAdmissionTest PS4ExecutableImageTest GuestRIPStoreAdmissionTest PS4VirtualMemoryRandomTest; do
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
  -I MaxPS4-iOS/Native \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  MaxPS4-iOS/Native/maxps4_guest_elf.cpp \
  MaxPS4-iOS/Native/maxps4_ps4_libkernel.cpp \
  MaxPS4-iOS/Native/maxps4_ps4_virtual_memory.cpp \
  MaxPS4-iOS/Native/maxps4_ps4_threads.cpp \
  MaxPS4-iOS/Native/maxps4_ps4_linker.cpp \
  "MaxPS4-iOS/Tests/$test.cpp" -o "$build_dir/$test"
python3 scripts/run-bounded.py 30 "$build_dir/$test.txt" "$build_dir/$test"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -c \
  MaxPS4-iOS/Tests/PS4SystemServicesABITest.c -o "$build_dir/services-abi.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -c \
  MaxPS4-iOS/Tests/PS4GuestMemoryABITest.c -o "$build_dir/memory-abi.o"
