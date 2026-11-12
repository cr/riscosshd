# riscosshd: an SSH server for RISC OS 5

An SSH server for RISC OS 5, built on [libssh](https://www.libssh.org/)
(LGPL 2.1) and [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0),
cross-compiled with GCCSDK.  It runs as a Wimp task with an icon-bar icon
and accepts public-key logins only.  `ssh host '*Cat'` runs a command,
`ssh host` gives an interactive `*` command line in a TaskWindow, and
`sftp`/`scp` move files.  Keys are managed in a desktop window, including
an import that logs in to another machine and fetches its public keys.

Status: exec, interactive shell, SFTP/scp and the desktop front end all
work against RISC OS 5.30 in RPCEmu (2026-11-11).  See "Roadmap".

**Security model.**  Any key listed in `Choices:SSHd.authkeys` has full
control of the machine: every `*command`, and over SFTP every filing
system, `!Boot` included.  Only the user name `riscos` is accepted (the
`user` setting; empty accepts any), and it is a filter, not a factor: the
key decides.  That is the natural fit for a single-user RISC OS box; treat
the key list accordingly.

## Why it looks the way it does

RISC OS has BSD sockets (the Internet module, reachable through UnixLib)
and a command-line interpreter, but none of the Unix machinery an SSH
server normally leans on:

- **No `fork`, no processes.**  There is one application at a time in the
  application slot; everything else is a module or a Wimp task.  The server
  therefore handles all connections in a single cooperative poll loop, each
  connection being a `session_ctx` driven by one libssh `ssh_event`.
- **No pty.**  The RISC OS equivalent is the **TaskWindow** module: a child
  task whose keyboard input and VDU output are relayed as Wimp messages
  (`Message_TaskWindow_Input` / `_Output`).  A `shell` request starts such
  a child with `Wimp_StartTask`; its output goes to the SSH channel and the
  client's keystrokes are sent to it as input (`src/backend_taskwindow.c`).
  `exec` requests use the same mechanism with `-quit`: the command is
  written to a scratch Obey file that sets a system variable to 1 before
  it and 0 after it, so a RISC OS error or an Escape leaves 1 and becomes
  the SSH exit status, and output streams as the command produces it.
  Refusals and errors go to the channel's stderr.  In text mode `exec`
  runs through `OS_CLI` inside the server's own task with VDU output
  redirected to a file (`OS_ChangeRedirection`).
- **No Unix shell.**  `exec` requests are RISC OS `*commands`; an
  interactive session is the TaskWindow's own `*` command line.  In text
  mode (`-t`) a line editor feeding `OS_CLI` stands in for it.
- **Sockets are system-wide handles**, not per-process descriptors, and
  the machine is cooperatively multitasked.  The server is a Wimp task
  that services libssh from null events, so the desktop keeps going; in
  text mode (`-t`) it polls with short timeouts and yields with
  `OS_UpCall 6`.
- **Entropy** comes from the CryptRandom module, wired into mbedTLS through
  `mbedtls_hardware_poll()`.

The SSH transport is libssh's, unmodified; everything RISC OS specific is
in `src/riscos*.c` and the backends.

## Layout

```
src/main.c            argument parsing, config dir, text-mode loop
src/server.c          ssh_bind, sessions, timeouts, libssh callbacks, output pump
src/backend.h         session backend interface
src/backend_taskwindow.c  exec and shell: TaskWindow child relayed over Wimp messages
src/backend_sftp.c    sftp subsystem: libssh's SFTP server on the channel
src/backend_exec.c    text mode: one OS_CLI command, captured output
src/backend_shell.c   text mode: line-mode * prompt on the same mechanism
src/wimp.c            Wimp front end: icon bar, menu, info, log and keys windows
src/keyfetch.c        import public keys from another machine as an SSH client
src/auth.c            authorized_keys parsing, matching, add and remove
src/hostkey.c         ed25519 host key load/generate
src/riscos.c          OS_File, OS_GSTrans, OS_CLI capture, signals
src/riscos_entropy.c  mbedTLS hooks: CryptRandom entropy, ms clock, start-up check
src/riscos_sftp.c     filetype helpers used by the libssh SFTP patch
src/riscos_compat.c   statvfs stub for libssh's SFTP server
src/log.c             log lines to the window, the log file, or stderr in text mode
tests/                host-side unit test of auth.c (make check)
riscos/!SSHd          !Run, !Boot, !Help, sprites (binary is added by the build)
riscos/RiscPkg        Control file for the PackMan package
cmake/                GCCSDK toolchain file
deps/                 mbedTLS config, libssh patch, compat header (sources/sysroot git-ignored)
docker/, tools/       GCCSDK container, dependency build, engine and RPCEmu helpers
docs/                 toolchain research, RPCEmu notes, audit
```

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
`SSH_USER`.  `make install` finds the RPCEmu data directory with the most
recent `rpclog.txt` under `~/Documents/RPCEmu`; set `RPCEMU_HOSTFS` to
override.  The same `make` runs in GitHub Actions
(`.github/workflows/build.yml`).  `docs/toolchain.md` explains why
build.riscos.online is not used.

## Package

`dist/SSHd_<version>.zip` is a RiscPkg/PackMan package: `Apps/Network/!SSHd`
plus `RiscPkg/Control` and `RiscPkg/Copyright`.  The version comes from
the `Version:` field in `riscos/RiscPkg/Control`.  The component flags
`(Movable LookAt Run)` let PackMan offer to run `!SSHd` at boot and to
have `!Boot` look at it, so the server can come up with the desktop.

## SFTP and scp

The `sftp` subsystem is libssh's built-in SFTP server running on the
channel (`src/backend_sftp.c`), so `sftp` and modern `scp` (OpenSSH 9 and
later, which transfers over SFTP) both work.  Paths are what UnixLib makes
of RISC OS names: the boot disc root is `/HostFS::HostFS.$/` on RPCEmu,
`dir.file/ext` appears as `/dir/file.ext`, and clients start in the
directory given with `-r` (default `<Boot$Dir>.^`).  Permissions, owners
and timestamps in `setstat`/`fsetstat` are accepted and ignored, since
RISC OS has nothing to map them to.  The legacy `scp -O` protocol is not
supported.

**Filetypes** follow the HostFS/LanManFS convention.  A name ending in
`,xxx` (three hex digits) refers to the file without that suffix and, on
upload or rename, gives it filetype `xxx`: `put prog prog,ffb` stores a
BASIC file called `prog`.  Otherwise the extension is looked up through
MimeMap, so `notes.txt` becomes Text and `pic.png` a PNG.  Listings show
`,xxx` on files whose type the extension does not already imply, and a
client may use either spelling: `get prog,ffb` and `get prog` are the
same file.

## Desktop front end

`!SSHd` is a Wimp task.  The icon-bar menu offers **Info...**, a
tail-style **Log Window**, a **Keys...** window, **Options...** (opens the
settings file in your text editor) and **Quit**.  The Keys
window shows the server's host key, lists the authorized keys with a
Delete button each, and adds keys three ways: a writable field, a text
file dropped from the Filer or an application, or **Import**, which logs
in to another machine as an SSH client (libssh's client side, password or
keyboard-interactive auth, host key fingerprint confirmed in a dialogue)
and imports that machine's `~/.ssh/id_*.pub`.  It opens by itself when no
key is configured.  `-t` runs the text mode in a TaskWindow instead.

## Installing and running

1. Install `dist/SSHd_<version>.zip` with PackMan, or copy `build/!SSHd`
   to the machine (`make install` does that for RPCEmu, see
   `docs/rpcemu.md`).  The package depends on CryptRandom; without PackMan,
   install that module yourself (`!Run` soft-loads
   `System:Modules.CryptRand`).
2. Double-click `!SSHd`.  An icon appears on the icon bar, the host key is
   generated into `Choices:SSHd.hostkey` on first start, and the server
   listens on port 22.
3. With no key configured the Keys window opens by itself: paste a public
   key, drop a `.pub` file on it, or use Import with the account and
   password of your usual machine.  Keys live in `Choices:SSHd.authkeys`.

```bash
ssh riscos@riscos-host '*Cat'   # run a command, output streams back
ssh riscos@riscos-host          # interactive * command line; Ctrl-D ends it
sftp riscos@riscos-host         # or scp; paths look like /HostFS::HostFS.$/Public
```

Commands run in a TaskWindow child, so they behave as in Edit's task
windows: line input, no Wimp applications.  A command is one line of an
Obey file: control characters and more than 1000 characters are refused,
and Obey substitutes `%0`..`%9` (with nothing) and `%%` (with `%`) before
the command runs.  The exit status is 0 when
the command completed and 1 when it raised a RISC OS error or was
interrupted.  A child that prints faster than the client reads is
suspended and resumed with the TaskWindow module's own messages.  If the
server itself dies with a fatal error, any children keep running and show
in the Task Manager as "SSH session"; quit them there.  Connections that
do not authenticate within 60 s, or sit logged in without a channel for
120 s, are dropped; at most 8 connections are held at once.  All three
are settings.

Settings live in `Choices:SSHd.Options` (Options... on the menu opens
it), written on first run with every setting commented out at its
default in the `name value  # comment` form, and survive package
upgrades:
`port`, `bind`, `root`, `user` (one accepted user name, default
`riscos`, empty for any), `shell`/`exec`/`sftp` on or off,
`auth-timeout`, `idle-timeout`, `max-sessions`, `banner` and `motd`
(text files `Choices:SSHd.Banner`, sent before login, and
`Choices:SSHd.Motd`, printed when a shell starts), `filetype` (for
uploads with no `,xxx` suffix and no MimeMap match, default `ffd`),
`verbose`, `log` and `log-size`.  The log is also written to
`Choices:SSHd.log`.  A failed start keeps the icon on the bar
with the log window open rather than showing a modal box, so a boot
sequence that runs `!SSHd` is never stalled.  After an upgrade, Quit the
running instance and start the new one.

## Roadmap

1. Publish the package in a PackMan index (`riscos/RiscPkg/Control` is ready; the zip layout is
   what `package-zip` produces).
2. Window-size and colour handling for the TaskWindow session.

## Licence

MIT for this repository.  The binary statically links libssh (LGPL 2.1),
mbedTLS (Apache 2.0) and GCCSDK's UnixLib (LGPL with exceptions); the
package's `RiscPkg/Copyright` carries all four texts, and the build
scripts here let anyone relink against a modified libssh or UnixLib as the
LGPL requires.
