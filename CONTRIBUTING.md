# Contributing

## What you need

- Docker, Podman or Colima: the build runs in the GCCSDK container
  (`make engine` says which engine is used and starts Colima if needed).
- For `make check`, libssh on the host (`brew install libssh`, or
  `apt install libssh-dev pkg-config`); CI runs it on every push.
- For trying changes, RPCEmu with RISC OS 5 and the port forward from
  `docs/rpcemu.md`.

## The loop

```bash
make            # dependencies (first time), the server, the package
make install    # copy !SSHd into RPCEmu's HostFS
make smoke      # *Time and an sftp listing against localhost:2222
make check      # host-side unit test of the key parser
```

Restart `!SSHd` in the emulator after `make install`.  `make PROFILE=1`
builds a server that logs where its time goes; `make WERROR=1` builds as
CI does, with warnings as errors.

## Style

- C99 (gnu99), four spaces, K&R braces, as in `src/`.
- Comments in full sentences that say why, not what.
- British spelling in text people read (licence, authorised, sanitise);
  OpenSSH's own names stay as they are (`authorized_keys`).
- No new warnings: CI builds with `-Werror`.
- Desktop texts go in `riscos/!SSHd/Messages,fff`; log lines stay English.
- One change per commit, with a message that says what was wrong and what
  the change does.

## Releasing

1. Move the "Unreleased" notes in `CHANGELOG.md` under a heading
   `## <version> - <date>`.
2. Set `Version:` in `riscos/RiscPkg/Control` to the same version.
3. Commit, tag `v<version>`, push the commit and then the tag.

The tag's build publishes the PackMan index and a GitHub release whose
notes are that changelog section.  If the build fails nothing is
published: delete the tag (`git push --delete origin v<version>`), fix,
and tag again.  A repository ruleset stops a `v*` tag from being moved
to another commit once pushed; deleting one is still possible.

## Updating mbedTLS or libssh

See "Updating mbedTLS or libssh" in `docs/toolchain.md`;
`tools/libssh-patch.sh` keeps the libssh patch in a git tree while you
work on it.
