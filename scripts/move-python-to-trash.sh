#!/usr/bin/env bash
# One-time cleanup after the C++ rewrite: moves the old Python version into
# _trash/python-pifx/. Nothing is deleted, and tracked files are moved with `git mv`, so
# git records them as renames. Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/.."
T=_trash/python-pifx

mv_one() {
  local src="$1" dst="$2"
  [ -e "$src" ] || return 0
  mkdir -p "$(dirname "$dst")"
  if git ls-files --error-unmatch "$src" >/dev/null 2>&1; then
    git mv "$src" "$dst"
  else
    mv "$src" "$dst"
  fi
  echo "moved $src -> $dst"
}

mv_one pifx             "$T/pifx"            # the Python package (+ its __pycache__)
mv_one web              "$T/web"             # the web UI
mv_one requirements.txt "$T/requirements.txt"
mv_one pifx.service     "$T/pifx.service"    # replaced by packaging/pifx.service
for f in tests/*.py; do
  [ -e "$f" ] && mv_one "$f" "$T/$f"
done
mv_one tests/__pycache__ "$T/tests/__pycache__"
mv_one tests/shots       "$T/tests/shots"

echo
echo "done - review with: git status"
