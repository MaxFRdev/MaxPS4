#!/usr/bin/env bash
# Build the existing SwiftUI application with one coherent, embedded Darwin runtime.
set -euo pipefail
SOURCE=${1:?Usage: scripts/build-fexcore-app.sh AETHER_CHECKOUT}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
mkdir -p build/frameworks build/diagnostics
bash scripts/build-fexcore-darwin-ios.sh "$SOURCE" build/fex-app-ios --runtime
FRAMEWORK=build/frameworks/MaxPS4FEXRuntime.framework
mkdir -p "$FRAMEWORK/Headers" "$FRAMEWORK/Modules"
cp build/fex-app-ios/libMaxPS4FEXRuntime.dylib "$FRAMEWORK/MaxPS4FEXRuntime"
xcrun install_name_tool -id '@rpath/MaxPS4FEXRuntime.framework/MaxPS4FEXRuntime' "$FRAMEWORK/MaxPS4FEXRuntime"
cp MaxPS4-iOS/Native/maxps4_fex_runtime.h "$FRAMEWORK/Headers/MaxPS4FEXRuntime.h"
cp MaxPS4-iOS/Native/maxps4_jit_memory.h "$FRAMEWORK/Headers/"
cat > "$FRAMEWORK/Modules/module.modulemap" <<'MAP'
framework module MaxPS4FEXRuntime { umbrella header "MaxPS4FEXRuntime.h" export * }
MAP
python3 - "$FRAMEWORK" <<'PY'
import plistlib, sys
from pathlib import Path
p=Path(sys.argv[1]) / 'Info.plist'
p.write_bytes(plistlib.dumps(dict(CFBundleIdentifier='com.enermaxfr.maxps4.fexruntime',
    CFBundleExecutable='MaxPS4FEXRuntime', CFBundleName='MaxPS4FEXRuntime',
    CFBundlePackageType='FMWK', CFBundleShortVersionString='0.3.0', CFBundleVersion='132',
    CFBundleSupportedPlatforms=['iPhoneOS'], MinimumOSVersion='18.0')))
PY
# Ad-hoc signing authenticates the edited Mach-O only; SideStore must re-sign it.
codesign --force --sign - "$FRAMEWORK"
# Preserve the existing exact logo as an Xcode app-icon asset.
ASSETS=MaxPS4-iOS/Resources/Assets.xcassets
mkdir -p "$ASSETS/AppIcon.appiconset"
sips -z 1024 1024 MaxPS4-iOS/Resources/MaxPS4ExactIcon.png \
  --out "$ASSETS/AppIcon.appiconset/AppIcon.png" >/dev/null
python3 - "$ASSETS" <<'PYICON'
import json, struct, sys
from pathlib import Path
root=Path(sys.argv[1])
# Reject missing, malformed or incorrectly sized icon output before xcodebuild.
icon = root / 'AppIcon.appiconset/AppIcon.png'
data = icon.read_bytes()
if len(data) < 33 or data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
    raise SystemExit('Generated MaxPS4 app icon is not a valid PNG header')
width, height = struct.unpack_from('>II', data, 16)
if (width, height) != (1024, 1024):
    raise SystemExit(f'Generated MaxPS4 app icon must be 1024x1024, got {width}x{height}')
(root/'Contents.json').write_text(json.dumps({'info':{'author':'xcode','version':1}}))
(root/'AppIcon.appiconset/Contents.json').write_text(json.dumps({
 'images':[{'filename':'AppIcon.png','idiom':'universal','platform':'ios','size':'1024x1024'}],
 'info':{'author':'xcode','version':1}}))
PYICON
xcodegen generate --spec MaxPS4-iOS/project.yml
xcodebuild -project MaxPS4-iOS/MaxPS4.xcodeproj -scheme MaxPS4 \
  -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
  -derivedDataPath build/xcode-fex CODE_SIGNING_ALLOWED=NO \
  build \
  2>&1 | tee build/diagnostics/fex-app-build.log
mkdir -p build/Payload
rm -rf build/Payload/MaxPS4.app
cp -R build/xcode-fex/Build/Products/Release-iphoneos/MaxPS4.app build/Payload/
# Generate the optional animated guest from the same native fixture verified by CI.
ANIMATED=build/Payload/MaxPS4.app/PlayableAnimatedDemo
mkdir -p "$ANIMATED"
build/fex-app-host/Bin/maxps4-ps4-linked-test --animated-playable-bundle "$ANIMATED"
cp MaxPS4-iOS/Resources/PlayableDemo/maxps4.json "$ANIMATED/"
test -s "$ANIMATED/app.elf" && test -s "$ANIMATED/provider.prx" && test -s "$ANIMATED/maxps4.json"
# Record the tested source revision before signing, for shareable device proof.
python3 - <<'PYREVISION'
import plistlib, subprocess
from pathlib import Path
path = Path('build/Payload/MaxPS4.app/Info.plist')
info = plistlib.loads(path.read_bytes())
info['MaxPS4SourceRevision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
# Bind the packaged app to the exact artwork checked into this source revision.
import hashlib
icon = Path('MaxPS4-iOS/Resources/MaxPS4ExactIcon.png')
info['MaxPS4AppIconSourceSHA256'] = hashlib.sha256(icon.read_bytes()).hexdigest()
path.write_bytes(plistlib.dumps(info))
PYREVISION
# Xcode strips development Headers/Modules during embedding. Re-seal the exact
# shipped framework, then verify it; SideStore will replace this ad-hoc identity.
EMBEDDED=build/Payload/MaxPS4.app/Frameworks/MaxPS4FEXRuntime.framework
codesign --force --sign - "$EMBEDDED"
codesign --verify --strict --verbose=2 "$EMBEDDED"
# The developer debugging entitlement requested by StikDebug, and extended
# virtual addressing: without it iOS left the app about 6.75 GiB of address
# space, and Mortal Kombat: Legacy Kollection ran out of memory in 5 GiB.
# This ad-hoc signature grants no provisioning rights; SideStore must supply a
# valid profile permitting both and re-sign BOTH app and framework.
codesign --force --sign - --entitlements MaxPS4-iOS/MaxPS4.entitlements build/Payload/MaxPS4.app
codesign --verify --strict --verbose=2 build/Payload/MaxPS4.app
codesign -d --entitlements - build/Payload/MaxPS4.app > build/diagnostics/fex-app-entitlements.plist
# Trace each post-signing verification so a failed command is actionable in CI.
trap 'status=$?; echo "IPA packaging failed at line ${LINENO}: ${BASH_COMMAND} (exit ${status})" >&2' ERR
set -x
# Xcode's selected sources must call the real opaque ABI, not a legacy build path.
xcrun nm -u build/Payload/MaxPS4.app/MaxPS4 > build/diagnostics/fex-app-imports.txt
xcrun otool -L build/Payload/MaxPS4.app/MaxPS4 > build/diagnostics/fex-app-libraries.txt
xcrun nm -gU "$FRAMEWORK/MaxPS4FEXRuntime" > build/diagnostics/fex-app-runtime-exports.txt
# Every public ABI entry must be exported by the embedded runtime. Only
# entries actually called by the Swift app are required in its undefined imports;
# the compiler may legitimately omit unused public functions.
for symbol in maxps4_fex_test_ps4_libkernel maxps4_fex_engine_open maxps4_fex_engine_initialize maxps4_fex_engine_cpuid maxps4_fex_engine_interpret_elf maxps4_fex_engine_run_elf maxps4_jit_begin maxps4_jit_cancel maxps4_jit_confirmation maxps4_jit_launch_failed maxps4_fex_engine_load_game maxps4_fex_engine_gnm_host_consumer maxps4_fex_engine_read_memory maxps4_fex_engine_videoout_target maxps4_fex_engine_request_stop; do
  if ! grep -q "_${symbol}$" build/diagnostics/fex-app-runtime-exports.txt; then
    echo "Missing FEXCore runtime export: ${symbol}" >&2
    exit 1
  fi
done
for symbol in maxps4_fex_engine_open maxps4_fex_engine_initialize maxps4_fex_engine_probe maxps4_fex_engine_load_game maxps4_fex_engine_run_orbis maxps4_fex_engine_gnm_host_consumer maxps4_jit_begin; do
  if ! grep -q "_${symbol}$" build/diagnostics/fex-app-imports.txt; then
    echo "Missing required app import: ${symbol}" >&2
    exit 1
  fi
done
(cd build && zip -qry MaxPS4-FEXCore-SideStore.ipa Payload)
python3 scripts/verify-fexcore-ipa.py build/MaxPS4-FEXCore-SideStore.ipa \
  | tee build/diagnostics/fex-app-ipa-verification.txt
