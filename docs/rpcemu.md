# Testing in RPCEmu

RPCEmu 0.9.x on macOS supports NAT networking (libslirp) and NAT port
forwarding, even though the online manual still says otherwise.  The
installed 0.9.4/0.9.5 source has `src/network-nat.c` and the rules UI
(`src/qt5/nat_list_dialog.cpp`).

## Addresses in NAT mode

| role            | address       |
|-----------------|---------------|
| RISC OS guest   | 10.10.10.10 (DHCP or manual, netmask 255.255.255.0) |
| gateway / host  | 10.10.10.2    |
| DNS             | 10.10.10.3    |

The "Easy-Start" RISC OS Direct bundle and the HardDisc4 5.30 image both
come with the EtherRPCEm driver and DHCP already configured; check with
`*ifconfig -a` in a TaskWindow.

## Port forwarding

Rules live in `rpc.cfg` next to the emulator's data (on this Mac
`~/Documents/RPCEmu/Data/rpc.cfg`, the folder whose `rpclog.txt` is
current).  Edit while RPCEmu is **not** running, or use
Settings → Networking → NAT port forwarding in the GUI:

```ini
[nat_port_forward_rules]
size=1
1\type=TCP
1\emu_port=22
1\host_port=2222
```

`type` is `TCP` or `UDP`; both ports must be 1–65535; up to 32 rules.
Then from the Mac:

```bash
ssh -p 2222 -o StrictHostKeyChecking=accept-new riscos@localhost '*Cat'
```

The user name is not checked by the server; the public key must be listed
in `!Boot.Choices.SSHd.authkeys` on the RISC OS side.

## Getting the app into the emulator

`make install` finds the RPCEmu data directory with the newest
`rpclog.txt` under `~/Documents/RPCEmu` (override with `RPCEMU_HOSTFS`),
copies `build/!SSHd` into its `hostfs/Apps/Network/` (the
`,ff8`/`,feb`/`,fff` suffixes carry the filetypes) and appends
`~/.ssh/id_ed25519.pub` to `hostfs/!Boot/Choices/SSHd/authkeys`.  Restart
`!SSHd` in the emulator afterwards; `make test` then checks a command and
an sftp listing through the forwarded port.
