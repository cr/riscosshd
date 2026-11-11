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
system, `!Boot` included.  The user name in the login is ignored.  That is
the natural fit for a single-user RISC OS box; treat the key list
accordingly.

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
  `exec` requests use the same mechanism with `-quit`, so output streams
  as the command produces it.  The TaskWindow module only reports that the
  child ended, not whether the command failed, so the SSH exit status is 0
  in that path.  In text mode `exec` runs through `OS_CLI` inside the
  server's own task with VDU output redirected to a file
  (`OS_ChangeRedirection`), which does report errors as status 1.
  On the desktop such commands are refused instead, because a command
  that hangs in the server's own task would take the server down.
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
src/riscos_entropy.c  mbedTLS hooks: CryptRandom entropy, ms clock
src/riscos_compat.c   statvfs stub for libssh's SFTP server
src/log.c             log lines to stderr or to the log window
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
ssh riscos-host '*Cat'          # run a command, output streams back
ssh riscos-host                 # interactive * command line; Ctrl-D ends it
sftp riscos-host                # or scp; paths look like /HostFS::HostFS.$/Public
```

Commands run in a TaskWindow child, so they behave as in Edit's task
windows: line input, no Wimp applications.  The exit status is always 0
on that path, because the TaskWindow module does not report a command's
error.  Connections that do not authenticate within 60 s, or sit logged
in without a channel for 120 s, are dropped; at most 8 connections are
held at once.

## Roadmap

1. Filetypes for files written over SFTP (MimeMap by extension, and the
   `,xxx` suffix convention).
2. Publish the package in a PackMan index (`riscos/RiscPkg/Control` is ready; the zip layout is
   what `package-zip` produces).
3. Window-size and colour handling for the TaskWindow session.

## Licence

MIT for this repository.  libssh is LGPL 2.1 (statically linked; the
build scripts here let anyone relink), mbedTLS is Apache 2.0.
