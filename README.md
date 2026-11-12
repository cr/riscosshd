# riscosshd: an SSH server for RISC OS 5

An SSH server for RISC OS 5, built on [libssh](https://www.libssh.org/)
(LGPL 2.1) and [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0),
cross-compiled with GCCSDK.  It runs as a Wimp task with an icon-bar icon
and accepts public-key logins only.  `ssh host '*Cat'` runs a command,
`ssh host` gives an interactive `*` command line in a TaskWindow, and
`sftp`/`scp` move files.  Keys are managed in a desktop window, including
an import that logs in to another machine and fetches its public keys.

Status: version 0.1.0, tested on RISC OS 5.30 in RPCEmu and installable
through PackMan.  See "Roadmap".

**Security model.**  Any key listed in `Choices:SSHd.authkeys` has full
control of the machine: every `*command`, and over SFTP every filing
system, `!Boot` included.  Only the user name `riscos` is accepted (the
`user` setting; empty accepts any), and it is a filter, not a factor: the
key decides.  That is the natural fit for a single-user RISC OS box; treat
the key list accordingly.

## Installation

SSHd needs RISC OS 5 (developed and tested against 5.30 in RPCEmu), a
configured network connection, and the CryptRandom module, which the
package pulls in.  The server listens on TCP port 22; if the machine sits
behind a router or an emulator's NAT, forward that port to reach it from
outside.

### With PackMan (recommended)

1. Open PackMan, choose **Sources...** from its menu, add this line and
   click **Update lists**:

       https://cr.github.io/riscosshd/pkg/index

2. Find **SSHd** in the Network section (or search for it), click
   **Install**, and accept CryptRandom as a dependency.
3. PackMan offers to run SSHd at startup, because the package carries the
   `Run` component flag.  Accept it for a server that is there after every
   boot; it can be changed later in PackMan's component settings or in
   Configure > Boot > Run.
4. Upgrades arrive through **Update lists** like any other package.  Quit
   the running SSHd from its icon bar menu before installing an upgrade,
   then start the new one.  Settings and keys live in `Choices:SSHd` and
   survive upgrades and removal.

### By hand

1. Download `SSHd_<version>.zip` from the
   [releases page](https://github.com/cr/riscosshd/releases).
2. Unpack it on RISC OS with something that turns the `,xxx` suffixes in
   the names into filetypes, such as !SparkFS; PackMan does this itself.
   Inside are `Apps/Network/!SSHd`, `RiscPkg/Control` and
   `RiscPkg/Copyright`.
3. Copy `!SSHd` to where you keep network applications, for example
   `$.Apps.Network`.
4. Install the CryptRandom module (PackMan, package "CryptRandom"); `!Run`
   loads `System:Modules.CryptRand` and SSHd refuses to start without it,
   since every key and nonce comes from it.
5. To run it at startup, drag `!SSHd` into Configure > Boot > Run.

### From source

`make` builds the package in a GCCSDK container (see "Building"), and
`make install` copies `build/!SSHd` into a local RPCEmu's HostFS
(`docs/rpcemu.md` has the port-forwarding setup).  Install CryptRandom
inside the emulator with PackMan.

### First start

1. Double-click `!SSHd`.  An icon appears on the icon bar; the first start
   generates the host key into `Choices:SSHd.hostkey` and writes
   `Choices:SSHd.Options` with every setting commented out at its default.
2. With no key configured the Keys window opens by itself.  Add your
   public key in one of three ways: paste the content of your `id_*.pub`
   file into the field and click **Add**; drop the `.pub` file on the
   window; or use **Import** with `user@host` (or `user@host:port`) and
   the password of a machine that already holds your keys.  Import logs in
   there as an SSH client, shows that machine's host key fingerprint for
   confirmation, and imports `~/.ssh/id_ed25519.pub`, `id_ecdsa.pub` and
   `id_rsa.pub`.  Keys live in `Choices:SSHd.authkeys`.

   Import needs an SSH server on the other machine (macOS: System
   Settings > General > Sharing > Remote Login; Linux: the openssh-server
   package).  Without one, copy `~/.ssh/id_ed25519.pub` to a disc, a
   network share or RPCEmu's HostFS folder and drop it on the Keys
   window.  `ssh-copy-id` does not work, as SSHd has no password logins.
3. The window's top line shows this server's own host key.  Compare it
   with what your client prints on its first connection.

```bash
ssh riscos@riscos-host '*Cat'   # run a command, output streams back
ssh riscos@riscos-host          # interactive * command line; Ctrl-D ends it
sftp riscos@riscos-host         # or scp; paths look like /HostFS::HostFS.$/Public
```

### If it does not start

The log window (SELECT on the icon) says why; the same lines go to
`Choices:SSHd.log`.  The usual causes: CryptRandom not installed, port 22
already taken by another server (or a second copy of SSHd, which reports
itself in a box and exits), or a network stack that is not up yet.  A
failed start keeps the icon on the bar with the log window open rather
than showing an error box, so a boot sequence that runs `!SSHd` never
stalls on it.

### Removing

Remove the package in PackMan, or delete `!SSHd` and its Boot > Run
entry.  `Choices:SSHd` (host key, authorized keys, settings, log) stays
until you delete it.

## Using it

**Commands and sessions** run in a TaskWindow child, so they behave as in
Edit's task windows: line input, no Wimp applications.  A command is one
line of an Obey file: control characters other than tab and more than
1000 characters are refused, and Obey substitutes `%0`..`%9` (with
nothing) and `%%` (with `%`) before the command runs.  The exit status is
0 when the command completed and 1 when it raised a RISC OS error or was
interrupted; refusals and errors go to stderr.  A child that prints
faster than the client reads is suspended until it catches up.  If the
server itself dies with a fatal error, any children keep running and
show in the Task Manager as "SSH session"; quit them there.

**Connections** that do not log in within 60 s, or sit logged in without
a session for 120 s, are dropped; at most 8 are held at once, and one
address may hold at most two that have not logged in yet.  The first
three numbers are settings.

**SFTP and scp** use libssh's SFTP server, so `sftp` and modern `scp`
(OpenSSH 9 and later, which transfers over SFTP) both work; the legacy
`scp -O` protocol does not.  Paths are what UnixLib makes of RISC OS
names: the boot disc root is `/HostFS::HostFS.$/` on RPCEmu, and
`dir.file/ext` appears as `/dir/file.ext`.  Clients start in the `root`
setting's directory, by default the boot disc.  Permissions, owners and
timestamps are accepted and ignored, as RISC OS has nothing to map them
to.  Files above 2 GB cannot be read or written past that point.

**Filetypes** follow the HostFS/LanManFS convention.  A name ending in
`,xxx` (three hex digits) refers to the file without that suffix and, on
upload or rename, gives it filetype `xxx`: `put prog prog,ffb` stores a
BASIC file called `prog`.  Otherwise the extension is looked up through
MimeMap, so `notes.txt` becomes Text and `pic.png` a PNG; a name MimeMap
does not know gets the `filetype` setting, Data by default.  Listings show
`,xxx` on files whose type the extension does not already imply, and a
client may use either spelling: `get prog,ffb` and `get prog` are the same
file.

**The desktop**: SELECT on the icon opens the log window, ADJUST the Keys
window, and the menu offers Info, Log Window, a Verbose tick, Keys...,
Options... (the settings file in your text editor) and Quit.  `!Help`
describes each part, and interactive help covers every icon and menu item.

**Settings** live in `Choices:SSHd.Options`, written on first run with
every setting commented out at its default, and survive package upgrades.
`!Help` lists them all.  They take effect at the next start; command line
options (`-p`, `-b`, `-r`, `-d`, `-t` for text mode in a TaskWindow, `-v`)
override them.

## Building

Everything is driven by the `Makefile`, which builds inside the GCCSDK
container.  It needs Docker, Podman, or Colima (`brew install colima
docker` on a Mac); `tools/container-engine.sh` picks whichever works and
`make engine` starts Colima if it is installed but stopped.  The GCCSDK
image is amd64 only and runs under emulation on Apple Silicon.

```bash
make            # image (once), dependencies (once), sshd, !SSHd, dist/SSHd_<version>.zip
make install    # copy !SSHd into a local RPCEmu HostFS and authorise ~/.ssh/id_ed25519.pub
make test       # run *Time and an sftp listing against localhost:2222
make check      # host-side unit test of the key parser (needs libssh on the host)
```

Other targets: `make image` rebuilds the toolchain container from
`docker/Dockerfile`; `make deps` cross-builds mbedTLS and libssh into
`deps/sysroot`; `make build` stops after the binary; `make package` makes
the PackMan zip; `make sprites` regenerates the icon from
`tools/mksprites.py`; `make clean` / `make distclean`.  Variables:
`ENGINE`, `IMAGE`, `RPCEMU_HOSTFS`, `PUBKEY`, `SSH_PORT`, `SSH_HOST`,
`SSH_USER`, and `PROFILE=1` for a server that logs where its time goes.  `make install` finds the RPCEmu data directory with the most
recent `rpclog.txt` under `~/Documents/RPCEmu` or `~/RPCEmu`; set
`RPCEMU_HOSTFS` to override.

GitHub Actions (`.github/workflows/build.yml`) runs `make check` and
`make` on every push and pull request.  `docs/design.md` explains the
design and the source layout, `docs/toolchain.md` the toolchain and why
build.riscos.online is not used.

## Package and publishing

`dist/SSHd_<version>.zip` is a RiscPkg/PackMan package:
`Apps/Network/!SSHd` plus `RiscPkg/Control` and `RiscPkg/Copyright`.  The
version comes from the `Version:` field in `riscos/RiscPkg/Control`, and
the component flags `(Movable LookAt Run)` let PackMan offer to run
`!SSHd` at boot.

A tag `v<version>` matching that field publishes a release: the same
workflow attaches the zip to a GitHub release and republishes the
PackMan index on GitHub Pages (`tools/mkindex.sh`).

## Roadmap

1. Get the package into the riscos.info autobuilder index, so it needs
   no extra source in PackMan.
2. Window-size and colour handling for the TaskWindow session.

## Licence

MIT for this repository.  The binary statically links libssh (LGPL 2.1),
mbedTLS (Apache 2.0) and GCCSDK's UnixLib (LGPL with exceptions); the
package's `RiscPkg/Copyright` carries all their licence texts (libssh's BSD notice included), and the build
scripts here let anyone relink against a modified libssh or UnixLib as the
LGPL requires.
