#!/bin/sh
# Copy the built !SSHd into an RPCEmu HostFS tree and authorise a public key.
#   tools/rpcemu-install.sh [hostfs-dir] [pubkey-file]
# Defaults: the HostFS tools/find-rpcemu.sh finds, and ~/.ssh/id_ed25519.pub.
# Does not touch rpc.cfg; add the port forward rule by hand (docs/rpcemu.md).
set -e
cd "$(dirname "$0")/.."
HOSTFS="${1:-$(sh tools/find-rpcemu.sh)}"
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
