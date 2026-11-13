# Test systems

SSHd runs on two machines during development, both with RISC OS Direct
but in different editions: the RPCEmu "Easy-Start" bundle is an
unofficial Risc PC port of an older Direct release, the Pi runs the
official, current Raspberry Pi edition.  They differ in ways that have
already caught bugs, so test a change on both when it touches paths,
packaging, networking or the boot sequence.  Plain RISC OS 5 from RISC
OS Open (the HardDisc4 image) is not among them and is untested.

| | RPCEmu (`make install`) | Raspberry Pi |
|---|---|---|
| RISC OS | Direct, Easy-Start bundle (Risc PC port): 5.27 ROM (19 Mar 2020), 5.20 boot hooks | Direct, Raspberry Pi edition: 5.31 (21 Jul 2024), 5.30 boot hooks |
| boot disc | `HostFS::HostFS.$`, a folder on the Mac | `SDFS::ROdirect.$`, a native SD card |
| `Boot$Dir` | `HostFS::HostFS.$.!Boot` | `SDFS::ROdirect.$.!BOOT` (upper case) |
| `Choices$Write` | `Boot:Choices` (a path variable) | `SDFS::ROdirect.$.!BOOT.Choices` |
| `Wimp$ScrapDir` | `HostFS::HostFS.$.!Boot.Resources.!Scrap.ScrapDirs.ScrapDir` | `SDFS::ROdirect.$.!BOOT.Resources.!Scrap.ScrapDirs.ScrapDir` |
| CryptRandom | `System:Modules.CryptRand` (2020) | `System:Modules.CryptRand`, the IPv6 build 0.20 (2024) |
| network | Acorn Internet stack, EtherRPCEm, NAT at 10.10.10.10, port 22 forwarded from 2222 | Direct's OBSD stack with NetManager, Wi-Fi `wb0`, DHCP, IPv4 and IPv6; unstable (below) |
| reach it | `ssh -p 2222 riscos@localhost` | `ssh riscos@riscos` |
| installed by | copying `build/!SSHd` | PackMan |

The Pi's Wi-Fi stack is unstable: at times it hangs the whole machine,
which then stops answering even ping, until it is taken off the network;
afterwards Wi-Fi finds no networks until a reboot.  A Pi that drops off
mid-test is therefore not by itself a sign of an SSHd fault; check the
log (`Choices:SSHd.log`) after the reboot before suspecting the server.

## What follows for the code

- **Filetypes.**  HostFS stores a filetype as a `,xxx` name suffix and
  reads it back as a type.  A native filing system keeps `!Run,feb` as a
  name.  So a zip with suffixed names installs on RPCEmu and fails on the
  Pi ("File ...!Run not found", 0.2.0-1).  `tools/mkzip.py` stores the
  types in the zip's Acorn extra field, the only place PackMan reads them.
  Testing the package on HostFS alone proves nothing about filetypes.
- **No fixed paths.**  The disc name, the case of `!BOOT` and the form of
  `Choices$Write` all differ, and PackMan lets the user move `!SSHd`.
  Every path is built from a system variable: `<SSHd$Dir>` (set by `!Boot`
  and `!Run` from `<Obey$Dir>`), `<Choices$Write>.SSHd`, `<Boot$Dir>.^`,
  `<Wimp$ScrapDir>`, `<Wimp$Scrap>` and `System:Modules`.  `ro_gstrans`
  refuses a variable that is not set, since GSTrans would expand it to
  nothing and leave a different path that still looks valid.
- **Path lengths.**  Native paths are longer than HostFS ones; the scrap
  file name (256 bytes) and a dropped file's name (212, the Wimp message
  limit) leave room for both.
- **SFTP paths** show the disc as UnixLib names it: `/HostFS::HostFS.$/`
  on RPCEmu, `/SDFS::ROdirect.$/` on the Pi.
