#!/usr/bin/env bash
# Downloads xwin and splats the MSVC CRT and Windows SDK that native/build.sh links against, at
# pinned versions, into $1 (default $XWIN_DIR, else ~/recomp/xwin). Re-running with the splat in
# place does nothing. Running it accepts Microsoft's license terms for those files
# (xwin --accept-license).
set -euo pipefail

out=${1:-${XWIN_DIR:-$HOME/recomp/xwin}}
xwin_version=0.10.0
xwin_sha256=d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88
manifest_version=17
crt_version=14.44.17.14
sdk_version=10.0.26100
stamp="xwin $xwin_version manifest $manifest_version crt $crt_version sdk $sdk_version"

[[ $(cat "$out/.pgr4-splat" 2>/dev/null) == "$stamp" ]] && { echo "$out: $stamp"; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tarball=xwin-$xwin_version-x86_64-unknown-linux-musl.tar.gz
curl -sSfL -o "$tmp/$tarball" "https://github.com/Jake-Shadle/xwin/releases/download/$xwin_version/$tarball"
echo "$xwin_sha256  $tmp/$tarball" | sha256sum --quiet -c
tar xzf "$tmp/$tarball" -C "$tmp"
"$tmp/${tarball%.tar.gz}/xwin" --accept-license --arch x86_64 --manifest-version "$manifest_version" \
  --crt-version "$crt_version" --sdk-version "$sdk_version" --cache-dir "$tmp/cache" \
  splat --output "$out"
echo "$stamp" > "$out/.pgr4-splat"
echo "$out: $stamp"
