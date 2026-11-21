#!/usr/bin/env python3
"""MaxPS4 automatic test run from a Windows (or macOS/Linux) computer.

One cycle: (optionally) install an IPA, ask MaxPS4 to start a game by itself,
launch MaxPS4, wait for the run's report in Documents/Rapports and copy it
here. Nothing is sent anywhere: the report stays in --out.

Needs: Python 3.9+, `pip install pymobiledevice3`, the iPhone plugged in by USB
and trusted, Developer Mode on. On iOS 17+, start the tunnel daemon first, in
an administrator terminal, and leave it running:
    python -m pymobiledevice3 remote tunneld
StikDebug must be set up on the iPhone (pairing file, VPN): MaxPS4 asks it for
the JIT by itself before the game starts.

Installing: an IPA must be signed for this iPhone. --sign uses zsign with the
user's own certificate (.p12) and provisioning profile; otherwise install the
IPA with SideStore first and run without --ipa.

Examples:
    python scripts/maxps4-autotest.py --title CUSA05379
    python scripts/maxps4-autotest.py --ipa MaxPS4.ipa --sign cert.p12 profile.mobileprovision --title CUSA05379
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.house_arrest import HouseArrestService
from pymobiledevice3.services.installation_proxy import InstallationProxyService

AUTOLAUNCH = "/Documents/MaxPS4-autolaunch.txt"
REPORTS = "/Documents/Rapports"


def run(*command):
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def find_bundle(lockdown, wanted):
    if wanted:
        return wanted
    apps = InstallationProxyService(lockdown=lockdown).get_apps(application_type="User")
    for bundle, info in apps.items():
        name = f"{info.get('CFBundleDisplayName', '')} {info.get('CFBundleName', '')}"
        if "MaxPS4" in name:
            return bundle
    sys.exit("MaxPS4 is not installed on this iPhone (or pass --bundle)")


def reports(afc):
    try:
        return {name for name in afc.listdir(REPORTS) if name.endswith(".txt") and "-en-cours" not in name}
    except Exception:
        return set()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--title", required=True, help="TITLE_ID of the game in MaxPS4's Jeux folder, or 'recent'")
    parser.add_argument("--ipa", help="IPA to install first")
    parser.add_argument("--sign", nargs=2, metavar=("P12", "PROFILE"), help="sign --ipa with zsign first")
    parser.add_argument("--password", default="", help="the .p12's password")
    parser.add_argument("--bundle", help="MaxPS4's bundle identifier (found by name otherwise)")
    parser.add_argument("--timeout", type=int, default=300, help="seconds to wait for the report")
    parser.add_argument("--out", default="reports", help="where to copy the report")
    args = parser.parse_args()

    lockdown = create_using_usbmux()
    if args.ipa:
        ipa = args.ipa
        if args.sign:
            ipa = os.path.join(tempfile.mkdtemp(), "MaxPS4-signed.ipa")
            run("zsign", "-k", args.sign[0], "-p", args.password, "-m", args.sign[1], "-o", ipa, "-z", "9", args.ipa)
        run(sys.executable, "-m", "pymobiledevice3", "apps", "install", ipa)
    bundle = find_bundle(lockdown, args.bundle)
    print("MaxPS4:", bundle, flush=True)

    afc = HouseArrestService(lockdown=lockdown, bundle_id=bundle, documents_only=True)
    before = reports(afc)
    afc.set_file_contents(AUTOLAUNCH, (args.title + "\n").encode())

    # Started fresh: the request is read when the home screen appears.
    run(sys.executable, "-m", "pymobiledevice3", "developer", "dvt", "launch", "--tunnel", "", "--kill-existing", bundle)

    deadline = time.time() + args.timeout
    relaunched = False
    while time.time() < deadline:
        time.sleep(5)
        new = sorted(reports(afc) - before)
        if new:
            os.makedirs(args.out, exist_ok=True)
            for name in new:
                local = os.path.join(args.out, name)
                with open(local, "wb") as file:
                    file.write(afc.get_file_contents(f"{REPORTS}/{name}"))
                print("Report:", local, flush=True)
            return
        # StikDebug may stay in front once the JIT is enabled: bring MaxPS4
        # back once (not killed), which goes on with the launch.
        if not relaunched and time.time() > deadline - args.timeout + 45:
            relaunched = True
            subprocess.run([sys.executable, "-m", "pymobiledevice3", "developer", "dvt", "launch", "--tunnel", "", "--no-kill-existing", bundle])
    sys.exit(f"No report after {args.timeout} s (Documents/Rapports unchanged)")


if __name__ == "__main__":
    main()
