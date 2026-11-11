#!/bin/sh
# Locate a local RPCEmu installation's HostFS directory for "make install".
# Honours RPCEMU_HOSTFS; otherwise picks, among ~/Documents/RPCEmu/*/hostfs,
# ~/Documents/RPCEmu/hostfs and ~/RPCEmu/hostfs, the one next to the most
# recently written rpclog.txt, i.e. the data directory the emulator uses.
if [ -n "$RPCEMU_HOSTFS" ]; then
    echo "$RPCEMU_HOSTFS"
    exit 0
fi
best=""; besttime=0
for d in "$HOME"/Documents/RPCEmu/*/ "$HOME"/Documents/RPCEmu/ "$HOME"/RPCEmu/; do
    [ -d "$d/hostfs" ] || continue
    t=0
    if [ -f "$d/rpclog.txt" ]; then
        t=$(stat -f %m "$d/rpclog.txt" 2>/dev/null || stat -c %Y "$d/rpclog.txt" 2>/dev/null || echo 0)
    fi
    if [ -z "$best" ] || [ "$t" -gt "$besttime" ]; then
        best="${d%/}/hostfs"; besttime=$t
    fi
done
if [ -n "$best" ]; then
    echo "$best"
    exit 0
fi
echo "No RPCEmu HostFS found; set RPCEMU_HOSTFS=/path/to/hostfs" >&2
exit 1
