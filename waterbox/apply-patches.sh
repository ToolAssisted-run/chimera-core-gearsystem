#!/bin/sh
# Applies patches/ to the extern/Gearsystem submodule's working tree. The
# submodule stays upstream's release as published; the patches are applied
# here, at every meson configure, and a patch already in the tree is left
# alone (a reverse check that succeeds means it is there).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
sub="$root/extern/Gearsystem"

# An empty submodule folder would make git answer for THIS repository and
# apply the patches to it: check the folder really is the submodule.
[ -f "$sub/src/GearsystemCore.cpp" ] || {
	echo "extern/Gearsystem is empty: git submodule update --init" >&2; exit 1; }
[ "$(git -C "$sub" rev-parse --show-toplevel)" = "$(cd "$sub" && pwd -P)" ] || {
	echo "extern/Gearsystem is not a git checkout of its own" >&2; exit 1; }

for p in "$root"/patches/*.patch; do
	if git -C "$sub" apply --reverse --check "$p" 2> /dev/null; then
		continue
	fi
	git -C "$sub" apply "$p"
	echo "applied $(basename "$p")"
done
