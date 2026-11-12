#!/bin/sh
# SPDX-License-Identifier: MIT
# Copy the built !SSHd into an RPCEmu HostFS tree and authorise a public key.
#   tools/rpcemu-install.sh [hostfs-dir] [pubkey-file]
# Defaults: RPCEMU_HOSTFS, else the HostFS find_hostfs finds; and
# ~/.ssh/id_ed25519.pub.  Does not touch rpc.cfg; add the port forward
# rule by hand (docs/rpcemu.md).
set -e
cd "$(dirname "$0")/.."

# Among ~/Documents/RPCEmu/*/hostfs, ~/Documents/RPCEmu/hostfs and
# ~/RPCEmu/hostfs, print the one next to the most recently written
# rpclog.txt, i.e. the data directory the emulator uses.
find_hostfs() {
    best=""; besttime=0
    for d in "$HOME"/Documents/RPCEmu/*/ "$HOME"/Documents/RPCEmu/ "$HOME"/RPCEmu/; do
        [ -d "$d/hostfs" ] || continue
        t=0
        if [ -f "$d/rpclog.txt" ]; then
            # GNU stat first (BSD stat rejects -c); GNU "stat -f %m" would
            # succeed with the mount point, which is why it must not be
            # tried first.
            t=$(stat -c %Y "$d/rpclog.txt" 2>/dev/null || stat -f %m "$d/rpclog.txt" 2>/dev/null || echo 0)
        fi
        if [ -z "$best" ] || [ "$t" -gt "$besttime" ]; then
            best="${d%/}/hostfs"; besttime=$t
        fi
    done
    [ -n "$best" ] || { echo "No RPCEmu HostFS found; set RPCEMU_HOSTFS=/path/to/hostfs" >&2; exit 1; }
    echo "$best"
}

HOSTFS="${1:-${RPCEMU_HOSTFS:-$(find_hostfs)}}"
PUBKEY="${2:-$HOME/.ssh/id_ed25519.pub}"
[ -d "build/!SSHd" ] || { echo "build/!SSHd missing: run make first" >&2; exit 1; }
[ -d "$HOSTFS/Apps" ] || { echo "$HOSTFS/Apps not found" >&2; exit 1; }
rm -rf "$HOSTFS/Apps/Network/!SSHd"
mkdir -p "$HOSTFS/Apps/Network"
cp -R "build/!SSHd" "$HOSTFS/Apps/Network/!SSHd"
CHOICES="$HOSTFS/!Boot/Choices/SSHd"
mkdir -p "$CHOICES"
if [ -f "$PUBKEY" ]; then
    touch "$CHOICES/authkeys"
    grep -qxF "$(cat "$PUBKEY")" "$CHOICES/authkeys" || cat "$PUBKEY" >> "$CHOICES/authkeys"
    echo "authorised $PUBKEY in $CHOICES/authkeys"
fi
echo "installed to $HOSTFS/Apps/Network/!SSHd"
