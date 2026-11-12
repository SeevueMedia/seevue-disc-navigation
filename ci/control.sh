#!/usr/bin/env bash
# Build an uncontained test control and a permissive SecurityManager JAR.
# Neither is packaged. X01 uses these to test the OS without Java denying first.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) platform=macos-aarch64; extra=(); rpath=@loader_path/../lib ;;
  Linux-aarch64) platform=linux-aarch64; extra=(-D_GNU_SOURCE); rpath='$ORIGIN/../lib' ;;
  Linux-x86_64) platform=linux-x86_64; extra=(-D_GNU_SOURCE); rpath='$ORIGIN/../lib' ;;
  *) exit 2 ;;
esac
work="$root/.build/$platform"
out="$work/control"
rm -rf "$out"
mkdir -p "$out/bin" "$out/lib" "$out/classes" "$out/src/org/videolan"
cp "$work/stage/lib/"* "$out/lib/"
cp -R "$work/stage/etc" "$out/"
cp "$work/stage/bin/bdnavd" "$out/bin/"
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter ${extra[@]+"${extra[@]}"} \
  -DBDNAVD_TEST_CONTROL -DSEEVUE_WORKER_VERSION='"0.4.0"' -I"$work/prefix/include" \
  "$root/src/bdnavd.c" "$root/src/readsvc.c" "$root/src/sandbox.c" \
  -L"$work/prefix/lib" -lbluray -lpthread -Wl,-rpath,"$rpath" -o "$out/bin/bdnavd-control"
jdk="$work/src/temurin-jdk"
[[ -d "$jdk/Contents/Home" ]] && jdk="$jdk/Contents/Home"
cat > "$out/src/org/videolan/BDJSecurityManager.java" <<'JAVA'
package org.videolan;
final class BDJSecurityManager extends SecurityManager {
    BDJSecurityManager(String disc, String persistent, String buda, int major) { }
    protected void setCacheRoot(String root) { }
    public void checkPermission(java.security.Permission permission) { }
    public void checkPermission(java.security.Permission permission, Object context) { }
}
JAVA
"$jdk/bin/javac" --release 8 -d "$out/classes" "$out/src/org/videolan/BDJSecurityManager.java"
jarfile="$out/lib/libbluray-j2se-1.5.0.jar"
"$jdk/bin/jar" uf "$jarfile" -C "$out/classes" org/videolan/BDJSecurityManager.class
if [[ "$platform" == macos-* ]]; then
  old="$(otool -L "$out/bin/bdnavd-control" | awk '/libbluray/ {print $1}')"
  install_name_tool -change "$old" "@rpath/$(basename "$old")" "$out/bin/bdnavd-control"
  codesign -s - -f "$out/bin/bdnavd-control" 2>/dev/null
fi
printf '%s\n' "$out"
