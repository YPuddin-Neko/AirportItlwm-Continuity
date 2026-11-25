#!/usr/bin/env python3
"""Run isolated AirportItlwm unit and sanitizer test suites."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = ROOT / "tests"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4, help="Parallel worker count")
    parser.add_argument("--pattern", default="test_*.py", help="Glob pattern under tests/")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    suites = sorted(TESTS_DIR.glob(args.pattern))
    if not suites:
        print("No test suites matched pattern:", args.pattern, file=sys.stderr)
        return 1

    env = dict(os.environ, PYTHONPATH=str(TESTS_DIR), PYTHONDONTWRITEBYTECODE="1")
    started = time.monotonic()

    def run_suite(suite: Path):
        t0 = time.monotonic()
        proc = subprocess.run(
            [sys.executable, str(suite)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )
        return suite.name, proc.returncode, time.monotonic() - t0, proc.stdout + proc.stderr

    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for index, (name, code, elapsed, output) in enumerate(pool.map(run_suite, suites), 1):
            status = "PASS" if code == 0 else "FAIL"
            print(f"[{index}/{len(suites)}] {status} {name} ({elapsed:.2f}s)", flush=True)
            if code != 0:
                failures.append((name, code, output))

    total_elapsed = time.monotonic() - started
    print(
        f"\nCompleted {len(suites)} suites in {total_elapsed:.2f}s: "
        f"{len(suites) - len(failures)} passed, {len(failures)} failed."
    )
    for name, code, output in failures:
        print(f"\n--- {name} (exit {code}) ---\n{output}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
