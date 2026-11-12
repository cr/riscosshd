# Changelog

Release notes for each package version.  A `v<version>` tag publishes the
section of the same version as its GitHub release notes; the build fails
when that section is missing.

## Unreleased

- SFTP renames never lose a file, case-only renames included.
- Unauthenticated clients can no longer make the server verify an unlimited
  number of signatures; RSA keys over 4096 bits are refused.
- Channel handling is safe when a multiplexing client closes and opens
  channels in one burst.
- Uploads are about 2.4 times faster.
- Login refusals are logged with their reason; disconnects tell the client
  why; SHA-1 RSA signatures are no longer accepted.
- The Keys window shows the command to connect with and fingerprints as
  ssh prints them; Return adds a key; the import fields are labelled.
- Interactive sessions: cursor keys no longer type junk, `exit` and Ctrl-D
  end the session, RISC OS screen codes are translated for the terminal.
- Desktop: Style Guide menu (Help..., Choices...), Info box, message
  boxes, Quit confirmation, a log window menu, centred windows, a greyed
  icon after a failed start, texts in a Messages file.
- The log file is rotated to `log-old` instead of being emptied.
- Reproducible builds.

## 0.1.0-3 - 2026-11-12

- First package that PackMan lists: the `OSDepends` field, which libpkg
  reads as a list of required modules, is gone.

## 0.1.0-2 - 2026-11-12

- First tagged release on the PackMan source; hidden by PackMan because
  of the `OSDepends` field.
