#!/usr/bin/env bash
# Runs the upstream OrcaSlicer CLI regression suite (orca-test-repo) against a ANANAS SLICER binary.
#
# Two adaptations, both explicit and verified:
#   1. The suite seeds every per-test --datadir with the BBL vendor bundle (conftest.py passes
#      "--vendor BBL" to parity/make_seed.py). ANANAS SLICER ships no BBL vendor, so make_seed.py
#      exits, the session fixture calls pytest.exit() and every xdist worker goes down
#      ("worker 'gwN' crashed"). The seed is switched to the Custom vendor; the edit must match
#      exactly once or this script fails.
#   2. ananas_slicer_orca_tests.py skips tests that need BBL presets or Bambu-only G-code, with
#      the reason in the report.
#
# The suite is pinned to one commit so a run is reproducible; bump ORCA_TEST_REPO_COMMIT
# deliberately after re-checking the skip rules against the new cases.
#
# usage: run.sh <ananas-slicer binary> <work directory>
set -euo pipefail

ORCA_TEST_REPO_URL="https://github.com/OrcaSlicer/orca-test-repo.git"
ORCA_TEST_REPO_COMMIT="02242a1c1a4003e9c683ed7531d6d74e5cccc943"

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <ananas-slicer binary> <work directory>" >&2
    exit 2
fi
BIN="$1"
WORK="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

[ -x "$BIN" ] || { echo "ERROR: binary not found or not executable: $BIN" >&2; exit 1; }

TEST_REPO="$WORK/orca-test-repo"
rm -rf "$TEST_REPO"
mkdir -p "$TEST_REPO"
git -C "$TEST_REPO" init --quiet || exit 1
git -C "$TEST_REPO" fetch --quiet --depth 1 "$ORCA_TEST_REPO_URL" "$ORCA_TEST_REPO_COMMIT" || exit 1
git -C "$TEST_REPO" checkout --quiet --detach FETCH_HEAD || exit 1

python3 - "$TEST_REPO/conftest.py" <<'PY' || exit 1
import sys

path = sys.argv[1]
old = '"--vendor", "BBL", "--vendor", "Custom"'
new = '"--vendor", "Custom"'
with open(path) as f:
    text = f.read()
count = text.count(old)
if count != 1:
    sys.exit("ERROR: expected exactly one BBL seed vendor list in %s, found %d; re-check the adaptation" % (path, count))
with open(path, "w") as f:
    f.write(text.replace(old, new))
PY

export PYTHONPATH="$HERE${PYTHONPATH:+:$PYTHONPATH}"
export PYTEST_ADDOPTS="-p ananas_slicer_orca_tests${PYTEST_ADDOPTS:+ $PYTEST_ADDOPTS}"
python3 "$TEST_REPO/run_test.py" "$BIN"
