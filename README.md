# riscosshd: an SSH server for RISC OS 5

A minimal SSH server for RISC OS 5, built on [libssh](https://www.libssh.org/)
(LGPL 2.1) and [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0),
cross-compiled with GCCSDK.  Public-key authentication only.  The first
milestone runs `*commands` for `ssh host '*Cat'` and offers a line-mode `*`
prompt for `ssh host`.

Status: milestones 1 (exec) and 2 (desktop front end, interactive shell
through TaskWindow) reached on 2026-11-11.  `ssh -p 2222 riscos@localhost '*Cat'`
against RISC OS 5.30 in RPCEmu returns the directory listing, RISC OS
errors come back with exit status 1, and the line-mode shell works over a
pipe.  See "Roadmap" for what is next.

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
  `exec` requests run through `OS_CLI` inside the server's own task with
  VDU output redirected to a file (`OS_ChangeRedirection`), which is read
  back and sent over the channel.
- **No Unix shell.**  `exec` requests are RISC OS `*commands`; an
  interactive session is the TaskWindow's own `*` command line.  In text
  mode (`-t`) a line editor feeding `OS_CLI` stands in for it.
- **Sockets are system-wide handles**, not per-process descriptors, and
  the machine is cooperatively multitasked.  The server runs in a
  TaskWindow so the desktop keeps going, polls with short timeouts, and the
  Internet module yields to the Wimp while waiting.
- **Entropy** comes from the CryptRandom module, wired into mbedTLS through
  `mbedtls_hardware_poll()`.

The SSH transport is libssh's, unmodified; everything RISC OS specific is
in `src/riscos*.c` and the backends.

## Layout

```
src/main.c            argument parsing, config dir, main loop
src/server.c          ssh_bind, sessions, libssh callbacks, output pump
src/backend.h         session backend interface
src/backend_exec.c    exec: one OS_CLI command, captured output
src/backend_shell.c   shell fallback: line-mode * prompt on the same mechanism
src/backend_taskwindow.c  shell: TaskWindow child task relayed over Wimp messages
src/wimp.c            Wimp front end: icon bar, menu, log and keys windows
src/keyfetch.c        import public keys from another machine as an SSH client
src/auth.c            authorized_keys parsing and matching
src/hostkey.c         ed25519 host key load/generate
src/riscos.c          OS_File, OS_GSTrans, OS_CLI capture
src/riscos_entropy.c  mbedTLS hooks: CryptRandom entropy, ms clock
riscos/!SSHd          !Run, !Boot, !Help (binary is added by the build)
riscos/RiscPkg        Control file for a PackMan / riscpkg package
cmake/                GCCSDK toolchain file
deps/                 dependency build config (sources and sysroot are git-ignored)
docker/, tools/       GCCSDK container and build scripts
docs/                 toolchain research and RPCEmu testing notes
```

## Building

Needs Docker (Colima on macOS works; the GCCSDK image is amd64 and runs
under emulation on Apple Silicon).

```bash
tools/build.sh
```

The first run downloads and cross-builds mbedTLS and libssh into
`deps/sysroot` (slow under emulation), then builds `build/!SSHd/sshd,ff8`
and `build/SSHd.zip`.  Later runs only rebuild the server.
`tools/build.sh deps` forces a dependency rebuild.  The same scripts run in
GitHub Actions (`.github/workflows/build.yml`).  `docs/toolchain.md`
explains why build.riscos.online is not used.

## Desktop front end

`!SSHd` is a Wimp task.  The icon-bar menu offers a tail-style **Log
window**, a **Keys** window and **Quit**.  The Keys window lists the
authorised keys with a Delete button each and adds keys three ways: a
writable field, a dropped text file, or **Fetch**, which logs in to another
machine as an SSH client (libssh's client side, password or
keyboard-interactive auth, host key fingerprint confirmed in a dialogue)
and imports that machine's `~/.ssh/id_*.pub`.  It opens by itself when no
key is configured.  `-t` runs the old TaskWindow text mode instead.

## Installing and running

1. Copy `build/!SSHd` to the RISC OS machine (HostFS in RPCEmu, see
   `docs/rpcemu.md`).
2. Put your public key(s), one per line, in `!Boot.Choices.SSHd.authkeys`
   (created empty on first run).
3. Install the CryptRandom module with PackMan if it is not present
   (`!Run` soft-loads `System:Modules.CryptRand`).
4. Double-click `!SSHd`.  It opens a TaskWindow, generates
   `Choices:SSHd.hostkey` on first start, and listens on port 22.

```bash
ssh riscos-host '*Cat'          # run a command
ssh riscos-host                 # line-mode * prompt, 'exit' to leave
```

Commands run synchronously inside the server task, so they must not be
interactive and must not start Wimp applications.

## Roadmap

1. PackMan package (`riscos/RiscPkg/Control` is ready; the zip layout is
   what `package-zip` produces).
2. SFTP (libssh has the server side; needs a RISC OS path mapping).
3. Window-size and colour handling for the TaskWindow session.

## Licence

MIT for this repository.  libssh is LGPL 2.1 (statically linked; the
build scripts here let anyone relink), mbedTLS is Apache 2.0.
