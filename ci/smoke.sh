#!/usr/bin/env bash
# Unpacks one platform's archives and checks that they run on this host:
# both workers start, send their hello and exit cleanly on an empty pipe, bdnavd resolves its
# shared libraries beside it, and the Java runtime starts.
#   ci/smoke.sh DIST_DIR PLATFORM
set -euo pipefail
dist="$1" platform="$2"
version="$(tr -d '[:space:]' < "$(dirname "$0")/../VERSION")"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror "$(dirname "$0")/bdj-retry-test.c" -o "$dir/bdj-retry-test"
"$dir/bdj-retry-test"
tar xf "$dist/seevue-disc-navigation-$version-$platform.tar.xz" -C "$dir"
tar xf "$dist"/seevue-disc-jre-*-"$platform".tar.xz -C "$dir"
(cd "$dist" && shasum -a 256 -c "SHA256SUMS-$platform" 2>/dev/null || sha256sum -c "SHA256SUMS-$platform")
test "$(cat "$dir/VERSION")" = "$version"
# Each worker's first frame is its protocol v5 hello (bdnavd adds its sandbox).
for worker in navd bdnavd; do
  "$dir/bin/$worker" </dev/null | grep -aq "{\"protocol\":5,\"worker\":\"$worker\",\"version\":\"$version\""
done
"$dir/jre/bin/java" -Xshare:auto -version
case "$platform" in
  macos-*) otool -L "$dir/bin/bdnavd" | grep -q '@rpath/libbluray' ;;
  linux-*) ldd "$dir/bin/bdnavd" | grep -q "$dir/bin/../lib/libbluray" ;;
esac
ls "$dir/lib/"libbluray-j2se-*.jar "$dir/etc/fonts/fonts.conf" "$dir/licenses/NOTICE.txt" >/dev/null
echo "$platform: smoke test passed"
