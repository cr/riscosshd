# Design notes

## Why it looks the way it does

RISC OS has BSD sockets (the Internet module, reachable through UnixLib)
and a command-line interpreter, but none of the Unix machinery an SSH
server normally leans on:

- **No `fork`, no processes.**  There is one application at a time in the
  application slot; everything else is a module or a Wimp task.  The server
  therefore handles all connections in a single cooperative poll loop, each
  connection being a `session_ctx` driven by one libssh `ssh_event`.  As
  libssh reads at most 4 KB per socket per poll, each pass polls again
  while data keeps arriving, within a few centiseconds; without that,
  uploads stopped at about 180 KB/s on RPCEmu, with it they reach about
  440 KB/s.  `make PROFILE=1` builds a server that logs where its time
  goes (`src/prof.h`).
- **No pty.**  The RISC OS equivalent is the **TaskWindow** module: a child
  task whose keyboard input and VDU output are relayed as Wimp messages
  (`Message_TaskWindow_Input` / `_Output`).  A `shell` request starts such
  a child with `Wimp_StartTask`; its output goes to the SSH channel and the
  client's keystrokes are sent to it as input (`src/backend_taskwindow.c`).
  `exec` requests use the same mechanism with `-quit`: the command is
  written to a scratch Obey file that sets a system variable to 1 before
  it and 0 after it, so a RISC OS error or an Escape leaves 1 and becomes
  the SSH exit status, and output streams as the command produces it.
- **No Unix shell.**  `exec` requests are RISC OS `*commands`; an
  interactive session is the TaskWindow's own `*` command line.  In text
  mode (`-t`), where there is no Wimp task to own TaskWindow children,
  `exec` runs through `OS_CLI` inside the server with VDU output
  redirected to a file (`OS_ChangeRedirection`), and a line editor feeding
  `OS_CLI` stands in for the shell.
- **Sockets are system-wide handles**, not per-process descriptors, and
  the machine is cooperatively multitasked.  The server is a Wimp task
  that services libssh from null events, so the desktop keeps going; in
  text mode it polls with short timeouts and yields with `OS_UpCall 6`.
- **Entropy** comes from the CryptRandom module, wired into mbedTLS through
  `mbedtls_hardware_poll()`; the server refuses to start without it.

libssh's protocol code is used as it is, apart from the changes in
`deps/patches/libssh-riscos.patch`, listed under "The libssh patch" in
`docs/toolchain.md`.  Everything else RISC OS specific is in
`src/riscos*.c` and the backends.

## Source layout

```
src/main.c            settings (Options file, command line), start-up, text-mode loop
src/server.c          ssh_bind, sessions, timeouts, libssh callbacks, output pump
src/buf.c             byte queue used for channel output, TaskWindow input, SFTP requests
src/prof.c            optional timing of the server loop (make PROFILE=1)
src/backend.h         session backend interface
src/backend_taskwindow.c  exec and shell: TaskWindow child relayed over Wimp messages
src/backend_sftp.c    sftp subsystem: libssh's SFTP server on the channel
src/backend_exec.c    text mode: one OS_CLI command, captured output
src/backend_shell.c   text mode: line-mode * prompt on the same mechanism
src/wimp.c            Wimp front end: icon bar, menu, Info, log and SSH Keys windows
src/keyfetch.c        import public keys from another machine as an SSH client
src/auth.c            authorised keys: parsing, matching, add and remove
src/hostkey.c         host key load, or ed25519 generation
src/riscos.c          OS_File, OS_GSTrans, OS_CLI capture, signals
src/riscos_entropy.c  mbedTLS hooks: CryptRandom entropy, start-up check, ms clock
src/riscos_sftp.c     filetype helpers used by the libssh SFTP patch
src/log.c             log ring for the window, the log file, stderr in text mode
tests/                host-side unit test of auth.c (make check)
riscos/!SSHd          !Run, !Boot, !Help, sprites (the binary is added by the build)
riscos/RiscPkg        Control file and licence texts for the PackMan package
cmake/                GCCSDK toolchain file
deps/                 mbedTLS config, libssh patch, riscos-compat headers (sources and sysroot git-ignored)
docker/               the GCCSDK container
tools/                dependency build, container engine, RPCEmu install, sprites,
                      Copyright file and PackMan index generators
.github/workflows/    CI build and test; releases on v<version> tags
docs/                 design, toolchain, RPCEmu notes, test systems, audit
```
