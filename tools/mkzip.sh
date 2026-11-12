#!/bin/sh
# SPDX-License-Identifier: MIT
# Zip a directory reproducibly: entries in sorted order, every file and
# directory dated SOURCE_DATE_EPOCH (default: now), fixed permissions, no
# extra attributes.  The same tree and epoch give the same bytes.
#   tools/mkzip.sh <dir> <zip>
set -e
dir="$1"; out="$2"
[ -n "$dir" ] && [ -n "$out" ] || { echo "usage: $0 <dir> <zip>" >&2; exit 1; }
epoch="${SOURCE_DATE_EPOCH:-$(date +%s)}"
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
rm -f "$out"
chmod -R u=rwX,go=rX "$dir"
find "$dir" -exec touch -h -d "@$epoch" {} +
cd "$dir"
find . -mindepth 1 | LC_ALL=C sort | zip -q -9 -X -@ "$out"
