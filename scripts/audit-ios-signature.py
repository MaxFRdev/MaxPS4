#!/usr/bin/env python3
"""Read-only audit of a built or re-signed .app on macOS; never re-signs it."""
import json
from pathlib import Path
import plistlib
import subprocess
import sys

KEYS = ('get-task-allow', 'com.apple.developer.kernel.increased-memory-limit',
        'com.apple.security.cs.allow-jit',
        'com.apple.security.cs.allow-unsigned-executable-memory',
        'dynamic-codesigning', 'application-identifier',
        'com.apple.developer.team-identifier')


def inspect(app):
    metadata = plistlib.loads((app / 'Info.plist').read_bytes())
    executable = app / metadata['CFBundleExecutable']
    signature = subprocess.run(['codesign', '--verify', '--strict', str(app)],
                               capture_output=True)
    ent = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(executable)],
                         capture_output=True)
    # Missing/unsigned data is unknown, never interpreted as false entitlements.
    signed = None
    if ent.returncode == 0 and ent.stdout.strip():
        signed = plistlib.loads(ent.stdout)
    profile = None
    profile_path = app / 'embedded.mobileprovision'
    if profile_path.exists():
        decoded = subprocess.run(['security', 'cms', '-D', '-i', str(profile_path)],
                                 capture_output=True)
        if decoded.returncode == 0:
            profile = plistlib.loads(decoded.stdout).get('Entitlements', {})
    return {
        'bundle_identifier': metadata['CFBundleIdentifier'],
        'signature_verification_exit': signature.returncode,
        'signed_entitlements': None if signed is None else {k: signed.get(k) for k in KEYS},
        'profile_entitlements': None if profile is None else {k: profile.get(k) for k in KEYS},
        'jit_execution_proven': False,
        'note': 'Profile grants are not runtime grants. Audit the exact installed signature after SideStore re-signing; no entitlement alone proves JIT.',
    }


if __name__ == '__main__':
    print(json.dumps(inspect(Path(sys.argv[1])), indent=2))
