#!/bin/bash
# Adds Wrapped to a clean Rockbox source tree.
#
#   ci/prepare-tree.sh ROCKBOX_TREE
#
# Applies rockbox-wrapped.patch (which registers the plugin in SOURCES and
# CATEGORIES) and checks the copy of wrapped.c inside the patch matches the
# repo's wrapped.c, so a forgotten patch update can't ship old code.
# Safe to run twice on the same tree.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
tree="${1:?usage: prepare-tree.sh ROCKBOX_TREE}"
cd "$tree"
if [ -f apps/plugins/wrapped.c ] && grep -q '^wrapped,apps' apps/plugins/CATEGORIES; then
  echo "tree already has Wrapped"
else
  git apply "$repo/rockbox-wrapped.patch"
  echo "applied rockbox-wrapped.patch to $(git describe --tags --always 2>/dev/null || echo tree)"
fi
if ! cmp -s apps/plugins/wrapped.c "$repo/wrapped.c"; then
  echo "ERROR: wrapped.c in rockbox-wrapped.patch is out of date with the repo's wrapped.c." >&2
  echo "       Regenerate the patch before building a release." >&2
  exit 1
fi
