#!/usr/bin/env python3
"""Run one CI check with a deadline and preserve its output even on timeout.

Use a regular log file: an inherited pipe in a diagnostic subprocess must not
keep tee/communicate waiting after the check itself has exited.
"""
import subprocess
import sys
from pathlib import Path


def main():
    if len(sys.argv) < 4:
        raise SystemExit("Usage: run-bounded.py SECONDS LOG COMMAND [ARG...]")
    seconds = float(sys.argv[1])
    if seconds <= 0:
        raise SystemExit("Deadline must be positive")
    path = Path(sys.argv[2])
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as output:
        try:
            result = subprocess.run(sys.argv[3:], timeout=seconds,
                                    stdout=output, stderr=subprocess.STDOUT)
            status = result.returncode
        except subprocess.TimeoutExpired:
            output.write(f"\nFAIL: deadline exceeded ({seconds:g} seconds); child terminated\n")
            status = 124
    print(path.read_text(errors="replace"), end="", flush=True)
    # Shells can truncate Windows exception statuses to eight bits, turning
    # some crashes into exit 0. Preserve failure across that boundary.
    if status < 0 or status > 255:
        print(f"FAIL: child exited with status {status}", flush=True)
        return 1
    return status


if __name__ == "__main__":
    raise SystemExit(main())
