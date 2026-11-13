#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Zip a directory for RISC OS, reproducibly.

    tools/mkzip.py <dir> <zip>

A file named with a ,xxx filetype suffix (!Run,feb) is stored under its
RISC OS name (!Run) with an Acorn extra field (0x4341, "ARC0") carrying the
filetype in its load address; files without a suffix are typed Text.  This
is the only place PackMan's libpkg takes a filetype from: it writes ,xxx
names through literally, which only HostFS reads back as filetypes.

Entries are in sorted order, every one dated SOURCE_DATE_EPOCH (default:
now), with fixed permissions, so the same tree and epoch give the same
bytes.
"""
import os
import re
import struct
import sys
import time
import zipfile

SUFFIX = re.compile(r"^(.*),([0-9a-fA-F]{3})$")
TEXT = 0xFFF
SECONDS_1900_TO_1970 = 2208988800
ATTR = 0x13                     # owner read and write, public read


def acorn_extra(filetype, epoch):
    """The extra field Info-ZIP and SparkFS write on RISC OS: the load and
    exec addresses hold the filetype and the 5-byte time in centiseconds
    since 1900."""
    cs = (epoch + SECONDS_1900_TO_1970) * 100
    load = 0xFFF00000 | (filetype << 8) | ((cs >> 32) & 0xFF)
    execute = cs & 0xFFFFFFFF
    return struct.pack("<HH4sIIII", 0x4341, 20, b"ARC0", load, execute, ATTR, 0)


def entries(top):
    """Every directory and file below top, relative, in byte order."""
    found = []
    for root, dirs, files in os.walk(top):
        rel = os.path.relpath(root, top)
        for name in dirs:
            found.append((os.path.normpath(os.path.join(rel, name)), True))
        for name in files:
            found.append((os.path.normpath(os.path.join(rel, name)), False))
    return sorted(found, key=lambda e: e[0].encode())


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkzip.py <dir> <zip>")
    top, out = sys.argv[1], sys.argv[2]
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH") or time.time())
    date = time.gmtime(epoch)[:6]
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w") as z:
        for rel, is_dir in entries(top):
            if is_dir:
                info = zipfile.ZipInfo(rel + "/", date)
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
                continue
            head, leaf = os.path.split(rel)
            match = SUFFIX.match(leaf)
            filetype = int(match.group(2), 16) if match else TEXT
            info = zipfile.ZipInfo(os.path.join(head, match.group(1)) if match else rel, date)
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.extra = acorn_extra(filetype, epoch)
            with open(os.path.join(top, rel), "rb") as f:
                z.writestr(info, f.read(), compresslevel=9)


if __name__ == "__main__":
    main()
