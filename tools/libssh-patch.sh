#!/bin/sh
# Work on deps/patches/libssh-riscos.patch in a git tree.
#
#   tools/libssh-patch.sh edit   unpack the pinned libssh into deps/patchwork,
#                                commit it as "pristine", apply the patch
#   tools/libssh-patch.sh save   write the tree's changes back to the patch
#
# After a libssh version bump, edit applies the old patch with --merge, so
# conflicts show as markers to resolve before save.  The tarball must be in
# deps/dl (make deps fetches it).
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^LIBSSH_VER=//p' tools/build-deps.sh)
TREE=deps/patchwork/libssh-$VER
PATCH=deps/patches/libssh-riscos.patch
case "$1" in
edit)
    [ -f "deps/dl/libssh-$VER.tar.xz" ] || { echo "deps/dl/libssh-$VER.tar.xz missing: run make deps" >&2; exit 1; }
    [ ! -e "$TREE" ] || { echo "$TREE exists: save or remove it first" >&2; exit 1; }
    mkdir -p deps/patchwork
    tar -xJf "deps/dl/libssh-$VER.tar.xz" -C deps/patchwork
    git -C "$TREE" init -q
    git -C "$TREE" add -A
    git -C "$TREE" -c user.name=pristine -c user.email=pristine@localhost commit -q -m pristine
    (cd "$TREE" && patch -p1 --merge < "../../../$PATCH") || echo "resolve the conflicts in $TREE" >&2
    echo "edit $TREE, then: tools/libssh-patch.sh save"
    ;;
save)
    [ -d "$TREE/.git" ] || { echo "no $TREE: run edit first" >&2; exit 1; }
    git -C "$TREE" diff --no-color --src-prefix=a/ --dst-prefix=b/ > "$PATCH"
    echo "wrote $PATCH ($(grep -c '^+++ ' "$PATCH") files)"
    ;;
*)
    echo "usage: $0 edit|save" >&2
    exit 1
    ;;
esac
