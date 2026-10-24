#!/usr/bin/env python3
"""Slices one model through ananas-engine (a `slice` request over its JSON-RPC protocol) and writes the G-code.

    slice_via_engine.py --engine <bin> --profiles <resources/profiles> --vendor Voron \
        --printer <name> --process <name> --filament <name> --model <file> --output <gcode>

Used by tests/virtual-printer/run.sh when ANANAS_ENGINE_BIN is set. Standard library only.
Exit status: 0 sliced, 1 the engine refused or failed.
"""

import argparse
import json
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for name in ("engine", "profiles", "vendor", "printer", "process", "filament", "model", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()

    request = {"jsonrpc": "2.0", "id": 1, "method": "slice", "params": {
        "printer": args.printer, "process": args.process, "filament": args.filament,
        "model": args.model, "output": args.output}}
    proc = subprocess.run([args.engine, "--profiles", args.profiles, "--vendor", args.vendor],
                          input=json.dumps(request) + "\n", capture_output=True, text=True, timeout=600)
    response = None
    for line in proc.stdout.splitlines():
        message = json.loads(line)
        if message.get("id") == 1:
            response = message
        else:
            print("progress %3d%% %s" % (message["params"]["percent"], message["params"]["message"]))
    if proc.stderr:
        print(proc.stderr, file=sys.stderr)
    if response is None or "result" not in response:
        print("engine failed: %s" % (response or "no response (exit %d)" % proc.returncode), file=sys.stderr)
        return 1
    print(json.dumps(response["result"], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
