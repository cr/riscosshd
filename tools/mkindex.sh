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
#
# Also writes <site dir>/pkg/pointer, the "pointer file" RISC OS Open
# reads nightly to carry the packages in its own PackMan repository: each
# zip's URL without the scheme and host ("RISC OS Packaging Guide",
# Uploading, on the ROOL wiki).
set -e
site="$1"; base="$2"; shift 2
[ -n "$site" ] && [ -n "$base" ] && [ $# -gt 0 ] || {
    echo "usage: $0 <site dir> <base url> <zip>..." >&2; exit 1; }
mkdir -p "$site/pkg" "$site/packages"
index="$site/pkg/index"
pointer="$site/pkg/pointer"
html="$site/index.html"
# the path part of the base URL: https://host/a/b -> /a/b
path=$(printf '%s\n' "$base" | sed -e 's|^[a-zA-Z][a-zA-Z0-9+.-]*://[^/]*||' -e 's|/*$||')
: > "$index"
: > "$pointer"
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
    control=$(unzip -p "$zip" RiscPkg/Control | sed -e 's/[[:space:]]*$//')
    printf '%s\n' "$control" >> "$index"
    {
        echo "Size: $size"
        echo "MD5Sum: $sum"
        echo "URL: $base/packages/$name"
        echo
    } >> "$index"
    echo "$path/packages/$name" >> "$pointer"
    pkg=$(printf '%s\n' "$control" | sed -n 's/^Package: *//p')
    ver=$(printf '%s\n' "$control" | sed -n 's/^Version: *//p')
    desc=$(printf '%s\n' "$control" | sed -n 's/^Description: *//p')
    echo "<li><a href=\"packages/$name\">$pkg $ver</a>: $desc</li>" >> "$html"
done
echo "</ul></body></html>" >> "$html"
echo "index: $index, pointer: $pointer ($# package(s))"
