# Changelog

Release notes for each package version.  A `v<version>` tag publishes the
section of the same version as its GitHub release notes; the build fails
when that section is missing.

## 0.2.0-2 - 2026-11-13

- The package installs on native filing systems (SDFS, ADFS): files carry
  their RISC OS filetypes instead of ,xxx names, which only HostFS
  understood.  0.2.0-1 failed on a Raspberry Pi with "File ...!Run not
  found".

## 0.2.0-1 - 2026-11-13

- SFTP renames never lose a file, case-only renames included.
- Unauthenticated clients can no longer make the server verify an unlimited
  number of signatures; RSA keys over 4096 bits are refused.
- Channel handling is safe when a multiplexing client closes and opens
  channels in one burst.
- Uploads are about 2.4 times faster.
- Login refusals are logged with their reason; disconnects tell the client
  why; SHA-1 RSA signatures are no longer accepted.
- The Keys window is now SSH Keys, in four titled sections (Host Key
  Fingerprint, Public Key Import, Remote Import, Authorised Keys) with
  their instructions wrapped inside; it shows the command to connect with
  and fingerprints as ssh prints them; Return adds a key; the import
  fields are labelled.
- Interactive sessions: cursor keys no longer type junk, `exit` and Ctrl-D
  end the session, RISC OS screen codes are translated for the terminal.
  A pasted block ending in `exit` runs in full before the session ends.
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
