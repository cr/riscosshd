#!/bin/sh
# SPDX-License-Identifier: MIT
# Build a PackMan package index from package zips.
#
#   tools/mkindex.sh <site dir> <base url> <zip>...
#
# Writes <site dir>/pkg/index (one record per zip: the RiscPkg/Control
# inside it plus Size, MD5Sum and URL, as PackMan expects), copies the
# zips to <site dir>/packages/ and writes an index.html for people.  Users
# add <base url>/pkg/index under Sources in PackMan.
set -e
site="$1"; base="$2"; shift 2
[ -n "$site" ] && [ -n "$base" ] && [ $# -gt 0 ] || {
    echo "usage: $0 <site dir> <base url> <zip>..." >&2; exit 1; }
mkdir -p "$site/pkg" "$site/packages"
index="$site/pkg/index"
html="$site/index.html"
: > "$index"
{
    echo "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>RISC OS packages</title>"
    echo "<style>body{font-family:sans-serif;max-width:40em;margin:2em auto;padding:0 1em}code{background:#eee;padding:0 .2em}</style></head><body>"
    echo "<h1>RISC OS packages</h1>"
    echo "<p>Add this source in PackMan (Sources, then Update lists):</p>"
    echo "<p><code>$base/pkg/index</code></p><ul>"
} > "$html"
for zip in "$@"; do
    name=$(basename "$zip")
    cp "$zip" "$site/packages/$name"
    if command -v md5sum >/dev/null 2>&1; then
        sum=$(md5sum "$zip" | cut -d' ' -f1)
    else
        sum=$(md5 -q "$zip")
    fi
    size=$(wc -c < "$zip" | tr -d ' ')
    unzip -p "$zip" RiscPkg/Control | sed -e 's/[[:space:]]*$//' >> "$index"
    {
        echo "Size: $size"
        echo "MD5Sum: $sum"
        echo "URL: $base/packages/$name"
        echo
    } >> "$index"
    pkg=$(unzip -p "$zip" RiscPkg/Control | sed -n 's/^Package: *//p')
    ver=$(unzip -p "$zip" RiscPkg/Control | sed -n 's/^Version: *//p')
    desc=$(unzip -p "$zip" RiscPkg/Control | sed -n 's/^Description: *//p')
    echo "<li><a href=\"packages/$name\">$pkg $ver</a>: $desc</li>" >> "$html"
done
echo "</ul></body></html>" >> "$html"
echo "index: $index ($# package(s))"
