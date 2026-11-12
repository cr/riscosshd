# Toolchain

Findings as of 2026-11-10, verified by fetching the pages and running the
tools listed, not from memory.

## build.riscos.online (Gerph's RISC OS Build service)

- Alive: `https://build.riscos.online/` answers HTTP 200.  The front page's
  "Service status: Unknown" is a static placeholder.
- It is **not** a cross-compiler.  Builds run inside a RISC OS environment
  (Pyromaniac) on the server.  The `about.html` page lists the complete
  toolset: `amu`, `cc` (Norcroft), `libfile`, `link`, `objasm`, `squeeze`,
  `CMunge`, `perl` 5.0, `ResGen`, `bison`, `flex`, `sed`, `basic`,
  `jfpatch`, `p2c`.  **No GCC, no UnixLib, no CMake.**  GCCSDK version:
  none; it does not offer GCCSDK at all.
- Submission: a zip (or a single file) is POSTed to
  `http://json.build.riscos.online/build/json` (`curl -F source=@archive.zip`),
  or sent with the `riscos-build-online` client from
  `https://github.com/gerph/robuild-client`
  (`riscos-build-online -i source.zip -o output`).  The CI page
  (`ci-build.html`) shows a GitHub workflow that calls curl/jq; there is no
  dedicated GitHub Action.
- Project configuration: `.robuild.yaml` with a single `jobs.build` entry
  holding `env` (system variables), optional `dir`, `script` (a list of
  RISC OS `*` commands; any failure fails the build) and `artifacts`
  (exactly one `path`).  Flow YAML, block strings, anchors and tags are not
  supported.  Files use `,xxx` filetype suffixes.
- Limits: ephemeral filesystem, no boot sequence, no desktop, no network
  access documented from inside the build, 1 minute timeout on the plain
  HTTP endpoint, client timeout of up to 10 minutes.

**Conclusion:** the service cannot build this project.  libssh and mbedTLS
are CMake projects written against POSIX sockets and headers, which on
RISC OS exist only in GCCSDK's UnixLib.  Porting both to Norcroft and AMU
makefiles would be a project of its own.  The service remains an option
for a future Norcroft-only component (for example a small helper module).

## GCCSDK

- Source of truth: the GCCSDK Subversion repository
  (`svn://svn.riscos.info/gccsdk/trunk`).  `www.riscos.info` was returning
  503/403 during this research; `https://github.com/jhamby/riscos-gccsdk`
  is a readable mirror of GCCSDK plus the autobuilder.
- Release in use: **GCC 4.7.4 Release 5** (`arm-unknown-riscos-gcc`), with
  UnixLib and the `elf2aif` converter.  Default target is ARMv3 (no long
  multiply), which is why the mbedTLS inline assembly is disabled.
- Docker image: `riscosdotinfo/riscos-gccsdk-4.7` on Docker Hub, tags
  `latest`/`r7510` (2021), `r7687`/`ci` (2022).  **linux/amd64 only**, no
  arm64 build.  On Apple Silicon it runs under emulation; Colima with
  `binfmt: true` (the default) handles `--platform linux/amd64`.  The image
  is Ubuntu 20.04 with git, svn, wget and make but **no cmake**;
  `docker/Dockerfile` pins it by digest (`latest` as of 2026-11-11) and
  adds cmake 3.16, curl and zip on top.
- Paths inside the image: compiler in `/home/riscos/cross/bin`, UnixLib
  headers in `/home/riscos/cross/arm-unknown-riscos/include`, porting
  wrappers (`ro-make`, `ro-config`, `toolchain-riscos.cmake`) in
  `/home/riscos/env`.
- UnixLib provides BSD sockets (`sys/socket.h`, `select`, `poll.h`),
  `O_NONBLOCK`/`FIONBIO`, `gettimeofday`, `getopt`.  There is no
  `/dev/urandom` and no `getrandom`; entropy comes from the CryptRandom
  module (SWI `CryptRandom_Block` 0x51983, `CryptRandom_Byte` 0x51980).

## Autobuilder recipes

Checked in the autobuilder tree (`autobuilder/` in the mirror):

| package   | recipe                                  | notes |
|-----------|-----------------------------------------|-------|
| mbedtls   | `libraries/network/mbedtls/setvars`     | builds git master with `CFLAGS=-Dunix=1`, packages `!mbedTLS` |
| openssl   | none under that name (only via `AB_DISTRO=testing` libssl3 for libssh2) | |
| libssh    | **none** | only `libraries/libssh2` (client library, not usable for a server) |
| oslib     | `libraries/riscos/oslib/setvars`        | SVN trunk, ELF build |
| libgcrypt | `libraries/libgcrypt20`, `network/nettle` | GPL/LGPL, not needed |

CMake support exists in the autobuilder (`AB_CMAKE=yes`, toolchain file
`$GCCSDK_INSTALL_ENV/toolchain-riscos.cmake`), so CMake projects are a
known quantity for GCCSDK.

## Crypto backend decision

**mbedTLS 3.6 LTS** (Apache 2.0).  It builds with the GCCSDK toolchain
file after two small platform hooks that live in this repository:
`MBEDTLS_ENTROPY_HARDWARE_ALT` (CryptRandom) and
`MBEDTLS_PLATFORM_MS_TIME_ALT` (gettimeofday), both configured in
`deps/mbedtls_riscos_config.h` and implemented in `src/riscos_entropy.c`.
`MBEDTLS_NET_C` and `MBEDTLS_TIMING_C` are disabled because their sources
refuse to build off Unix/Windows and libssh does not use them.  To keep
the binary small the same file also leaves out TLS and every algorithm
libssh does not negotiate, and everything is built with function and
data sections so the link drops unused code; libssh is built without
group exchange.  OpenSSL was not needed.

## How dependencies are built

Option chosen: **build them in the same container job, from release
tarballs, into `deps/sysroot`** (git-ignored), driven by
`tools/build-deps.sh`.  Reasons: reproducible from pinned versions, no
binary blobs in git, the CI job and the local Colima build run the exact
same script.  Vendoring prebuilt `.a` files was rejected because the
GCC/UnixLib ABI would be frozen in the repository.

- The `Makefile` drives everything: `make image` builds the container
  from `docker/Dockerfile`, `make deps` runs `tools/build-deps.sh` inside
  it, `make build` configures and builds the server with
  `cmake/toolchain-riscos.cmake`, and `make package` produces
  `dist/SSHd_<version>.zip`.  `tools/container-engine.sh` picks Docker,
  Podman or Colima.
- `.github/workflows/build.yml` runs `make check` and `make ENGINE=docker`
  on an amd64 GitHub runner, where the image needs no emulation, caches
  the image and `deps/sysroot`, and uploads the package as an artifact;
  on a `v<version>` tag it also publishes the release and the PackMan
  index.

Pinned versions: mbedTLS 3.6.7, libssh 0.11.5.

## Porting notes (what actually broke, and the fix in this repo)

| symptom on RISC OS | cause | fix |
|---|---|---|
| mbedTLS: `#error "No mbedtls_ms_time available"` | not Unix/Windows | `MBEDTLS_PLATFORM_MS_TIME_ALT`, `mbedtls_ms_time()` in `src/riscos_entropy.c` |
| libssh: `strndup` implicit | UnixLib hides it without `_GNU_SOURCE` | `-D_GNU_SOURCE` in `tools/build-deps.sh` |
| libssh: `if_nametoindex` implicit | UnixLib declares it under `#if 0` | stub in `deps/patches/libssh-riscos.patch` |
| libssh: `B230400` undeclared | UnixLib's termios stops at 115200 | `#ifdef` guard in the patch |
| segfault in `libssh_constructor()` at start | libssh forces `-fPIC`; GCCSDK PIC globals go through a table only the shared-object manager fills in, absent in a static AIF | patch `DefineCMakeDefaults.cmake` to drop PIC; constructor/destructor attributes and `__thread` disabled with `-DHAVE_CONSTRUCTOR_ATTRIBUTE=0` etc. |
| "branch through zero" in `mbedtls_ctr_drbg_reseed_internal` | with the constructor gone, `ssh_init()` must run before any key operation | `main()` calls `ssh_init()` first |
| `ssh_init failed` | libssh picked pthread locking (UnixLib has `pthread.h`) but mbedTLS has no `MBEDTLS_THREADING_C` | `ssh_threads_set_callbacks(ssh_threads_get_noop())` before `ssh_init()` |
| `CryptRandom module missing or failing` | not in the 5.30 ROM; PackMan puts it in `System:Modules.CryptRand` and nothing loads it | the package depends on CryptRandom, `!Run` loads it if present, and `main()` probes it and refuses to start without it; the patch also makes `ssh_crypto_init()` fail when the DRBG cannot be seeded |
| "EMT trap" with no message | UnixLib maps hardware exceptions inside SWIs and RISC OS 5's branch-through-zero trap to SIGEMT | `src/riscos.c` installs a SIGEMT/SIGOSERROR handler that prints `_kernel_last_oserror()` |

`CryptRandom_Block` (R0 = buffer, R1 = length) works as documented; an
earlier suspicion that it faulted was the unseeded DRBG above.

The patch's SFTP server changes are commented where they are made; in
short: handlers for `fstat`, `fsetstat` (a size truncates), `rename`
(fails on an existing target) and `posix-rename@openssh.com` (replaces
it), which libssh does not implement; `open` honouring TRUNC and EXCL;
reads clamped to 32 KB and answered from the file size at or past the
end; writes beyond 2 GB refused, as `off_t` is 32-bit; `setstat`
ignoring modes, owner and times; a failed `realpath` being a client error
rather than a fatal one; `sftp_server_close_handles()` so a dropped
client's files are closed; and filetypes for new files and renames.

Filetypes are mostly UnixLib's work: with `__RISCOSIFY_FILETYPE_EXT` in
the application-defined `__riscosify_control` (in `src/main.c`),
`riscosify` strips `,xxx` and looks extensions up in MimeMap, `open`
types new files from that, and `unixify` adds `,xxx` in listings where
the extension does not imply the type.  Two small helpers in
`src/riscos_sftp.c` cover the rest: retyping after a rename, and the
`filetype` setting for new files whose names MimeMap does not know.  Two
UnixLib details that cost time: `__riscosify` returns a pointer to the
end of its output buffer, and `MimeMap_Translate`'s output code for a
RISC OS type number is 0 (1 means a type name string written to a buffer
in R3).
