#!/usr/bin/env python3
"""Checks the QR generator by DECODING what it draws.

Comparing against another implementation is not proof: two symbols can differ
module by module (different padding, a different mask chosen) and both be valid.
Only one question matters — does a real reader read it? So this check decodes
with OpenCV instead of comparing matrices.

This is NOT paranoia: a wrong QR raises no error. It draws, it looks fine on
screen, the finder patterns are in place, and no phone decodes it. That is
exactly what happened here — the format bits were in reverse order and only the
decoder caught it.

Setup (once):
    python3 -m venv /tmp/qrvenv && /tmp/qrvenv/bin/pip install opencv-python-headless numpy

Usage:
    cc tools/qr_dump.c src/qr.c -o /tmp/qr_dump -Isrc
    /tmp/qrvenv/bin/python tools/qr_check.py /tmp/qr_dump
"""
import subprocess
import sys

import cv2
import numpy as np

# One case per reason, not per whim:
CASES = [
    "https://nuvio.tv/tv-login?code=fa0010cad8b5d2f512e58646ab82ca6b",  # the real case
    "https://nuvio.tv/tv-login?code=00000000000000000000000000000000",  # data almost all identical
    "A",                    # the smallest possible input
    "HELLO WORLD",
    "12345678901234567",
    "x" * 53,               # the last that fits in version 3
    "y" * 54,               # the first that forces version 4
    "z" * 78,               # the last of version 4
    "w" * 106,              # version 5
    "k" * 134,              # version 6, the module's limit
]
# Above the limit the app MUST refuse rather than draw something truncated.
CASES_THAT_MUST_FAIL = ["q" * 135, ""]


def matrix(binary, text):
    output = subprocess.run([binary, text], capture_output=True, text=True)
    lines = output.stdout.strip().splitlines()
    if not lines or lines[0].startswith("did not fit"):
        return None
    return [l.strip() for l in lines[1:]]


def decode(m, scale=8, quiet=4):
    side = len(m)
    img = np.full((side + 2 * quiet, side + 2 * quiet), 255, dtype=np.uint8)
    img[quiet:quiet + side, quiet:quiet + side] = np.array(
        [[0 if c == "1" else 255 for c in line] for line in m], dtype=np.uint8
    )
    img = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST)
    text, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
    return text


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "/tmp/qr_dump"
    failures = 0
    for text in CASES:
        m = matrix(binary, text)
        label = text if len(text) <= 34 else f"{text[:10]}…({len(text)}B)"
        if m is None:
            print(f"FAIL   {label}: the app refused it, but this fits")
            failures += 1
            continue
        read = decode(m)
        if read == text:
            print(f"ok     {label}: {len(m)}x{len(m)} decoded identically")
        else:
            print(f"FAIL   {label}: {len(m)}x{len(m)} decoded as {read[:34]!r}")
            failures += 1
    for text in CASES_THAT_MUST_FAIL:
        if matrix(binary, text) is None:
            print(f"ok     ({len(text)}B): refused, as expected")
        else:
            print(f"FAIL   ({len(text)}B): should have been refused")
            failures += 1
    print("\n" + ("ALL READABLE" if not failures else f"{failures} FAILURE(S)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
