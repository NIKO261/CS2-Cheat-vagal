"""Run the focused bug-scan regressions against a source tree or rollback copy."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    results = []
    for name in ("lifecycle", "rendering", "jump_intent"):
        command = [sys.executable, str(Path(__file__).with_name(f"test_bugscan_{name}.py")),
                   "--root", str(root)]
        process = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        results.append(dict(suite=name, command=command, input=str(root), stdout=process.stdout,
                            stderr=process.stderr, exit_status=process.returncode))
        print(f"{name}: {'PASS' if process.returncode == 0 else 'FAIL'} (exit={process.returncode})")
    passed = sum(item["exit_status"] == 0 for item in results)
    summary = f"BUGSCAN: suites={len(results)} passed={passed} failed={len(results) - passed}"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(dict(summary=summary, results=results), ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
    print(summary)
    return int(passed != len(results))


if __name__ == "__main__":
    sys.exit(main())
