#!/usr/bin/env python3
"""Upload a G-code file to the rig's Moonraker, read its metadata and print it.

Runs inside the moonraker container against http://127.0.0.1:7125 (loopback is
Moonraker's only trusted client and no port is published). Steps:

1. /server/info: Klippy must be connected and ready.
2. POST /server/files/upload with print=true, as the slicer's "upload and
   print" sends it: Moonraker must report print_started or print_queued, so the
   slicer leaves the start to Moonraker. Only if it reports neither does this
   script fall back to POST /printer/print/start, as the slicer does, and fail.
   Klippy runs the file through virtual_sdcard. exclude_object must list the
   objects the file defines, print_stats must end "complete", and no G-code
   response may report an error or unknown command.
3. GET /server/files/metadata: slicer, estimated_time, thumbnails, layers.

Writes every API response used to a JSON file. Exit status: 0 pass, 1 fail,
2 usage.
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

BASE_URL = "http://127.0.0.1:7125"
HTTP_TIMEOUT_S = 30


class ApiError(Exception):
    pass


def request(method, path, query=None, body=None, headers=None):
    url = BASE_URL + path
    if query:
        url += "?" + urllib.parse.urlencode(query)
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors="replace")
        raise ApiError("%s %s -> HTTP %d: %s" % (method, path, exc.code, detail)) from exc
    except urllib.error.URLError as exc:
        raise ApiError("%s %s -> %s" % (method, path, exc.reason)) from exc


def upload(path, remote_name, start_print=False):
    boundary = "----klipslice-vp-" + uuid.uuid4().hex
    with open(path, "rb") as f:
        payload = f.read()
    parts = [
        b"--" + boundary.encode(),
        b'Content-Disposition: form-data; name="root"',
        b"",
        b"gcodes",
        b"--" + boundary.encode(),
        b'Content-Disposition: form-data; name="print"',
        b"",
        b"true" if start_print else b"false",
        b"--" + boundary.encode(),
        ('Content-Disposition: form-data; name="file"; filename="%s"' % remote_name).encode(),
        b"Content-Type: application/octet-stream",
        b"",
        payload,
        b"--" + boundary.encode() + b"--",
        b"",
    ]
    body = b"\r\n".join(parts)
    return request("POST", "/server/files/upload", body=body, headers={
        "Content-Type": "multipart/form-data; boundary=" + boundary,
        "Content-Length": str(len(body)),
    })


def wait_for(description, timeout_s, poll):
    """Call poll() until it returns a non-None value or the timeout expires."""
    deadline = time.monotonic() + timeout_s
    while True:
        value = poll()
        if value is not None:
            return value
        if time.monotonic() >= deadline:
            raise ApiError("timed out after %ds waiting for %s" % (timeout_s, description))
        time.sleep(0.5)


def defined_objects(gcode_path):
    names = []
    pattern = re.compile(r"^EXCLUDE_OBJECT_DEFINE\s+.*?\bNAME=(\S+)", re.IGNORECASE)
    with open(gcode_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            match = pattern.match(line)
            if match is not None:
                names.append(match.group(1).upper())
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcode", required=True, help="G-code file to upload")
    parser.add_argument("--remote-name", required=True, help="file name on the printer")
    parser.add_argument("--result", required=True, help="JSON file for the API responses")
    parser.add_argument("--print-timeout", type=int, required=True,
                        help="seconds the virtual print may take")
    args = parser.parse_args()
    if not os.path.isfile(args.gcode):
        print("check_moonraker: not a file: %s" % args.gcode, file=sys.stderr)
        return 2

    result = {"passed": False, "failures": [], "responses": {}}
    failures = result["failures"]
    responses = result["responses"]

    def fail(msg):
        failures.append(msg)

    try:
        info = request("GET", "/server/info")["result"]
        responses["server_info"] = info
        if info.get("klippy_state") != "ready":
            raise ApiError("Klippy state is %r, not 'ready'" % info.get("klippy_state"))

        responses["printer_info"] = request("GET", "/printer/info")["result"]
        responses["gcode_store_before"] = request(
            "GET", "/server/gcode_store", {"count": 1000})["result"]
        store_before = len(responses["gcode_store_before"]["gcode_store"])
        # The print: virtual_sdcard runs the file on the file-output MCU. Moonraker's upload
        # reply carries item, print_started and print_queued at top level (no result envelope).
        upload_reply = upload(args.gcode, args.remote_name, start_print=True)
        responses["upload"] = upload_reply
        if not (upload_reply.get("print_started") or upload_reply.get("print_queued")):
            fail("upload with print=true: Moonraker reported neither print_started nor print_queued")
            responses["print_start"] = request(
                "POST", "/printer/print/start", {"filename": args.remote_name})
        query = {"print_stats": "", "exclude_object": "", "virtual_sdcard": ""}

        def poll_objects():
            status = request("GET", "/printer/objects/query", query)["result"]["status"]
            if status["exclude_object"]["objects"]:
                return status
            if status["print_stats"]["state"] in ("complete", "error", "cancelled"):
                return status
            return None

        objects_status = wait_for("exclude_object objects", 60, poll_objects)
        responses["exclude_object"] = objects_status["exclude_object"]
        reported = [o["name"].upper() for o in objects_status["exclude_object"]["objects"]]
        expected = defined_objects(args.gcode)
        if not expected:
            fail("G-code defines no objects (no EXCLUDE_OBJECT_DEFINE)")
        if sorted(reported) != sorted(expected):
            fail("exclude_object objects %s != EXCLUDE_OBJECT_DEFINE names %s"
                 % (reported, expected))

        def poll_metadata():
            try:
                meta = request("GET", "/server/files/metadata",
                               {"filename": args.remote_name})["result"]
            except ApiError:
                return None
            return meta if "slicer" in meta or "estimated_time" in meta else None

        meta = wait_for("file metadata", 60, poll_metadata)
        responses["metadata"] = meta
        if not meta.get("slicer"):
            fail("metadata: no slicer detected")
        if not meta.get("estimated_time"):
            fail("metadata: estimated_time missing or zero")

        def poll_done():
            status = request("GET", "/printer/objects/query", query)["result"]["status"]
            if status["print_stats"]["state"] in ("complete", "error", "cancelled", "standby"):
                return status
            return None

        final = wait_for("print end", args.print_timeout, poll_done)
        responses["print_end"] = final
        if final["print_stats"]["state"] != "complete":
            fail("print_stats ended in state %r: %s" % (
                final["print_stats"]["state"], final["print_stats"].get("message", "")))

        store = request("GET", "/server/gcode_store", {"count": 1000})["result"]["gcode_store"]
        responses["gcode_store_during_print"] = store[store_before:] if store_before <= len(store) else store
        for entry in responses["gcode_store_during_print"]:
            msg = entry.get("message", "")
            if msg.startswith("!!") or "Unknown command" in msg:
                fail("G-code response during print: %s" % msg)

        responses["history_last"] = request(
            "GET", "/server/history/list", {"limit": 1, "order": "desc"})["result"]
        responses["configfile_warnings"] = request(
            "GET", "/printer/objects/query", {"configfile": "warnings"}
        )["result"]["status"]["configfile"]["warnings"]
    except (ApiError, KeyError, TypeError, ValueError) as exc:
        fail("%s: %s" % (type(exc).__name__, exc))

    result["passed"] = not failures
    with open(args.result, "w") as f:
        json.dump(result, f, indent=2)
    for msg in failures:
        print("FAIL: " + msg, file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
