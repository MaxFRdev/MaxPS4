#!/usr/bin/env bash
# Compile/link the complete matching Darwin runtime, never an app or IPA.
set -euo pipefail
SOURCE=${1:?Usage: bash scripts/build-fexcore-darwin-ios.sh AETHER_CHECKOUT BUILD_DIR}
OUT=${2:?Missing build directory}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SOURCE=$(cd "$SOURCE" && pwd)
FEX="$SOURCE/runtime/sources/fexcore-darwin"
test "$(git -C "$SOURCE" rev-parse HEAD)" = 3347fc13f19860855ce91b66356940989ea6b6a6
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
SYSTEM=iOS
DEPLOYMENT=18.0
TARGETS=(FEXCore_shared)
RUNTIME=OFF
# The device runtime (the IPA's FEXCore, HLE services, GNM and PM4 code) is
# optimized and tuned for the oldest iOS 18 device CPU (A12: ARMv8.3 with LSE
# atomics); a Debug build ran all of it at -O0 with FEX assertions. Not
# Release: FEX's LinkerGC then passes GNU-only --gc-sections/--strip-all/
# --as-needed to Apple's linker, and stripping would hide the host symbols
# the CPU profiler names. RelWithDebInfo is -O2 with frame pointers kept.
# Host runtimes for the macOS tests stay Debug, with assertions.
BUILD_TYPE=Debug
TUNE_CPU=none
if [[ "${3:-}" == --runtime || "${3:-}" == --host-runtime ]]; then
  RUNTIME=ON
  TARGETS+=(MaxPS4FEXRuntime maxps4-fex-runtime-test maxps4-ps4-linked-test)
fi
if [[ "${3:-}" == --runtime ]]; then
  BUILD_TYPE=RelWithDebInfo
  TUNE_CPU=apple-a12
fi
if [[ "${3:-}" == --host-runtime ]]; then
  SDK=$(xcrun --sdk macosx --show-sdk-path)
  SYSTEM=Darwin
  DEPLOYMENT=15.0
fi
python3 -B "$ROOT/MaxPS4-iOS/Tests/FEXDispatcherInitPatchTest.py" "$FEX"
python3 -B "$ROOT/scripts/patch-fexcore-dispatcher-init.py" "$FEX"
python3 -B "$ROOT/MaxPS4-iOS/Tests/FEXJITProviderPatchTest.py" "$FEX"
python3 -B "$ROOT/scripts/patch-fexcore-jit-provider.py" "$FEX"
python3 -B "$ROOT/scripts/patch-fexcore-lookup-cache.py" "$FEX"
python3 -B "$ROOT/scripts/patch-fexcore-ps4-cpuid.py" "$FEX"
# Patch only the disposable dependency checkout. No replacement symbols are defined.
python3 - "$FEX" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
old = '__builtin___clear_cache(static_cast<char*>(Begin), static_cast<char*>(Begin) + Length);'
for relative in ('CodeEmitter/CodeEmitter/Buffer.h', 'FEXCore/Source/Utils/ArchHelpers/Arm64.cpp'):
    path = root / relative
    text = path.read_text()
    if 'MaxPS4 Darwin cache API' in text:
        continue
    if text.count(old) != 1:
        raise ValueError(f'Unexpected cache invalidation context: {relative}')
    text = text.replace(old, 'sys_icache_invalidate(Begin, Length);', 1)
    path.write_text('// MaxPS4 Darwin cache API\n#include <libkern/OSCacheControl.h>\n' + text)
path = root / 'FEXCore/Source/Utils/AllocatorHooks.cpp'
text = path.read_text()
if '// MaxPS4 typed Darwin allocation' not in text:
    old = '  return ::aligned_alloc(a, s);'
    if text.count(old) != 1:
        raise ValueError('Unexpected libc-backed aligned_alloc implementation')
    text = text.replace(old, '''#if defined(__APPLE__)
  // FEXAlloc requests alignof(char/uint32_t). Darwin requires at least
  // pointer alignment; POSIX allocation also permits arbitrary byte sizes.
  if (a == 0 || (a & (a - 1)) != 0) return nullptr;
  void* Ptr {};
  const size_t Alignment = a < sizeof(void*) ? sizeof(void*) : a;
  return ::posix_memalign(&Ptr, Alignment, s) == 0 ? Ptr : nullptr;
#else
  return ::aligned_alloc(a, s);
#endif''', 1)
    path.write_text('// MaxPS4 typed Darwin allocation\n' + text)
PY
cmake -S "$FEX" -B "$OUT" -G Ninja \
  -DCMAKE_SYSTEM_NAME="$SYSTEM" -DCMAKE_SYSTEM_PROCESSOR=arm64 \
  -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DMAXPS4_BUILD_RUNTIME="$RUNTIME" -DMAXPS4_ROOT="$ROOT" \
  -DMAXPS4_AETHER_SOURCE="$SOURCE" \
  -DTUNE_CPU="$TUNE_CPU" -DTUNE_ARCH=generic \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="$ROOT/cmake/FEXCoreIOSLinkage.cmake" \
  -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_unordered_dense=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_range-v3=ON \
  -DBUILD_FEXCORE_ONLY=ON -DBUILD_FEXCORE_SMOKE=OFF \
  -DBUILD_TESTING=OFF -DBUILD_FEX_LINUX_TESTS=OFF -DBUILD_THUNKS=OFF \
  -DBUILD_FEXCONFIG=OFF -DENABLE_GDB_SYMBOLS=OFF -DENABLE_LTO=OFF \
  -DENABLE_CCACHE=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
  -DENABLE_FEX_ALLOCATOR=OFF -DENABLE_VIXL_DISASSEMBLER=OFF \
  -DENABLE_ZYDIS=OFF -DOVERRIDE_VERSION=MaxPS4-Darwin \
  -DOVERRIDE_HASH=f2b679f6028ce1c38875233aecfcf5d3f8ebecec \
  2>&1 | tee "$OUT/configure.log"
cmake --build "$OUT" --target "${TARGETS[@]}" --parallel 3 --verbose 2>&1 | tee "$OUT/build-link.log"
python3 - "$OUT" <<'PY'
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
commands = json.loads((out / 'compile_commands.json').read_text())
sources = sorted({row['file'].split('/fexcore-darwin/')[-1] for row in commands
                  if '/fexcore-darwin/FEXCore/Source/' in row['file']})
assert len([p for p in sources if '/JIT/' in p]) == 11
assert 'FEXCore/Source/Interface/Core/CPUBackend.cpp' in sources
(out / 'source-manifest.txt').write_text('\n'.join(sources) + '\n')
PY
LIB="$OUT/FEXCore/Source/libFEXCore.dylib"
test -s "$LIB"
file "$LIB" | tee "$OUT/mach-o.txt"
xcrun lipo -archs "$LIB" | tee "$OUT/architectures.txt"
xcrun nm -u "$LIB" > "$OUT/undefined.txt"
xcrun nm -gU "$LIB" > "$OUT/defined.txt"
xcrun otool -L "$LIB" > "$OUT/libraries.txt"
xcrun otool -Iv "$LIB" > "$OUT/indirect.txt"
# A strict dylib may retain SDK imports; FEXCore imports must all be defined.
if xcrun nm -u "$LIB" | grep -E 'FEXCore|LogMan'; then
  echo 'Unresolved FEXCore runtime import' >&2
  exit 1
fi
echo "PASS complete strict $SYSTEM ARM64 Darwin linkage; device execution NOT tested" | tee "$OUT/link-status.txt"
if [[ "$RUNTIME" == ON ]]; then
  NATIVE="$OUT/libMaxPS4FEXRuntime.dylib"
  file "$NATIVE" > "$OUT/native-mach-o.txt"
  xcrun nm -u "$NATIVE" > "$OUT/native-undefined.txt"
  xcrun otool -L "$NATIVE" > "$OUT/native-libraries.txt"
  if xcrun nm -u "$NATIVE" | grep -E 'FEXCore|LogMan|maxps4_'; then
    echo 'Native engine contains unresolved runtime symbols' >&2
    exit 1
  fi
  if [[ "$SYSTEM" == Darwin ]]; then
    python3 "$ROOT/scripts/run-bounded.py" 60 "$OUT/ps4-linked-tests.txt" "$OUT/Bin/maxps4-ps4-linked-test"
    python3 "$ROOT/scripts/run-bounded.py" 10 "$OUT/fixture-export.txt" "$OUT/Bin/maxps4-ps4-linked-test" \
      "$OUT/ps4-libkernel-demo.elf" "$OUT/ps4-libkernel-combined.elf"
    python3 "$ROOT/scripts/run-bounded.py" 60 "$OUT/runtime-tests.txt" "$OUT/Bin/maxps4-fex-runtime-test"
  fi
fi
