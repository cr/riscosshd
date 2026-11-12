#!/bin/sh
# SPDX-License-Identifier: MIT
# Assemble RiscPkg/Copyright for the package: SSHd's licence followed by
# those of the statically linked libraries.   mkcopyright.sh SRCDIR OUTFILE
set -e
src="$1"; out="$2"
{
    echo "SSHd is distributed under the MIT licence:"
    echo
    cat "$src/LICENSE"
    echo
    echo "The sshd binary statically links the following libraries.  The build"
    echo "scripts at $(sed -n 's/^Homepage: *//p' "$src/riscos/RiscPkg/Control")"
    echo "rebuild it from source, which lets anyone relink against a modified"
    echo "libssh or UnixLib as the LGPL requires."
    for f in libssh-lgpl-2.1 libssh-bsd mbedtls-apache-2.0 unixlib; do
        echo
        echo "=================================================================="
        case $f in
            libssh-bsd) echo "libssh (https://www.libssh.org/): the parts under the BSD licence" ;;
            libssh-*)  echo "libssh (https://www.libssh.org/), GNU Lesser General Public License 2.1" ;;
            mbedtls-*) echo "mbedTLS (https://github.com/Mbed-TLS/mbedtls), Apache License 2.0" ;;
            unixlib)   echo "UnixLib (GCCSDK, https://www.riscos.info/)" ;;
        esac
        echo "=================================================================="
        echo
        cat "$src/riscos/RiscPkg/licences/$f.txt"
    done
} > "$out"
