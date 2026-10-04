#!/usr/bin/env python3
"""Minimal test runner (no pytest needed): python3 tests/run_tests.py [pattern]"""
import importlib
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))


def main() -> int:
    pattern = sys.argv[1] if len(sys.argv) > 1 else ""
    failed = 0
    total = 0
    for path in sorted(HERE.glob("test_*.py")):
        if pattern and pattern not in path.stem and not any(
                pattern in line for line in path.read_text().splitlines() if line.startswith("def test_")):
            continue
        try:
            mod = importlib.import_module(path.stem)
        except ImportError as e:          # e.g. scipy missing on a dev laptop
            print(f"  skip  {path.stem}: {e}")
            continue
        whole_module = pattern in path.stem
        for name in dir(mod):
            if not name.startswith("test_") or (pattern not in name and not whole_module):
                continue
            total += 1
            try:
                getattr(mod, name)()
                print(f"  ok    {path.stem}.{name}")
            except Exception:  # noqa: BLE001
                failed += 1
                print(f"  FAIL  {path.stem}.{name}")
                traceback.print_exc()
    print(f"\n{total - failed}/{total} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
