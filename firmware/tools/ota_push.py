#!/usr/bin/env python3
"""Push a firmware.bin to the display over Wi-Fi and verify it took.

    DEVICE_KEY=... ota_push.py --host agent-display.local .pio/build/waveshare7/firmware.bin

Reads the build's identity from the image (ESP-IDF app descriptor), skips the
upload if the device already runs it, POSTs it to /ota, then polls /status until
the device is back on the new build and has confirmed it (first good fetch
from the hub). Reports a rollback if the old build comes back instead. Stdlib only, so
it runs under PlatformIO's own Python.
"""
import argparse
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request

APP_DESC_OFFSET = 0x20  # image header (24) + first segment header (8)
APP_DESC_MAGIC = 0xABCD5432


def read_app_desc(path):
    with open(path, "rb") as f:
        f.seek(APP_DESC_OFFSET)
        desc = f.read(0x100)
    (magic,) = struct.unpack_from("<I", desc, 0)
    if magic != APP_DESC_MAGIC:
        sys.exit(f"{path}: no ESP-IDF app descriptor (magic {magic:#x}); is this firmware.bin?")
    cstr = lambda off, n: desc[off:off + n].split(b"\0", 1)[0].decode(errors="replace")
    return {
        "version": cstr(0x10, 32),
        "project": cstr(0x30, 32),
        "time": cstr(0x50, 16),
        "date": cstr(0x60, 16),
        "elf_sha256": desc[0x90:0xB0].hex(),
    }


def get_status(host, timeout=4):
    with urllib.request.urlopen(f"http://{host}/status", timeout=timeout) as r:
        return json.load(r)


def describe(status):
    return f"{status.get('version')} built {status.get('built')} {status.get('built_time')} in {status.get('slot')} ({status.get('ota_state')})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("--host", required=True, help="device hostname or IP")
    ap.add_argument("--force", action="store_true", help="upload even if the device already runs this build")
    ap.add_argument("--boot-wait", type=int, default=90, help="seconds to wait for the device to come back")
    ap.add_argument("--confirm-wait", type=int, default=120, help="seconds to wait for the new image to confirm itself")
    args = ap.parse_args()

    key = os.environ.get("DEVICE_KEY")
    if not key:
        sys.exit("DEVICE_KEY is not set")
    new = read_app_desc(args.firmware)
    size = os.path.getsize(args.firmware)
    print(f"image:  {new['project']} {new['version']} built {new['date']} {new['time']}, {size} bytes, sha {new['elf_sha256'][:12]}")

    try:
        before = get_status(args.host)
    except (urllib.error.URLError, OSError, ValueError) as e:
        sys.exit(f"{args.host}: /status unreachable ({e}); is the display on and on the network?")
    print(f"device: {describe(before)}, sha {before.get('elf_sha256', '')[:12]}, up {before.get('uptime_s')} s")
    if before.get("update_in_progress"):
        sys.exit("device reports an update already in progress")
    if before.get("elf_sha256") == new["elf_sha256"] and not args.force:
        print("device already runs this build; nothing to do (use --force to re-flash)")
        return 0

    with open(args.firmware, "rb") as f:
        body = f.read()
    req = urllib.request.Request(
        f"http://{args.host}/ota", data=body, method="POST",
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/octet-stream"},
    )
    print(f"uploading {size} bytes to http://{args.host}/ota ...")
    started = time.time()
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            reply = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"device refused the image: HTTP {e.code} {e.read().decode(errors='replace')}")
    except (urllib.error.URLError, OSError) as e:
        sys.exit(f"upload failed: {e}")
    print(f"written to {reply.get('slot')} in {time.time() - started:.0f} s; device is rebooting")
    if reply.get("elf_sha256") and reply["elf_sha256"] != new["elf_sha256"]:
        sys.exit("device reports a different image than the one sent; aborting the wait")

    deadline = time.time() + args.boot_wait
    time.sleep(3)
    status = None
    while time.time() < deadline:
        try:
            status = get_status(args.host)
            if status.get("uptime_s", 1e9) < 300:
                break
        except (urllib.error.URLError, OSError, ValueError):
            pass
        time.sleep(2)
    if not status or status.get("uptime_s", 1e9) >= 300:
        sys.exit(f"device did not come back within {args.boot_wait} s; check it over USB (pio device monitor)")
    if status.get("elf_sha256") != new["elf_sha256"]:
        sys.exit(f"ROLLED BACK: device came back on {describe(status)} (reset: {status.get('reset_reason')})")
    print(f"booted: {describe(status)}, reset {status.get('reset_reason')}, ip {status.get('ip')}")

    deadline = time.time() + args.confirm_wait
    while time.time() < deadline:
        try:
            status = get_status(args.host)
        except (urllib.error.URLError, OSError, ValueError):
            status = {}
        if status.get("elf_sha256") and status["elf_sha256"] != new["elf_sha256"]:
            sys.exit(f"ROLLED BACK after boot: device is on {describe(status)} (reset: {status.get('reset_reason')})")
        if status.get("ota_state") == "valid":
            print("confirmed: the new build reached the hub and is now the permanent image")
            return 0
        time.sleep(3)
    sys.exit(f"new build is running but has not confirmed itself within {args.confirm_wait} s "
             f"(state {status.get('ota_state')}); it will roll back unless it reaches the hub soon")


if __name__ == "__main__":
    sys.exit(main())
