#!/usr/bin/env bash
# Builds the disc workers and the trimmed Java runtime for this host.
#
#   ./build.sh            build into .build/<platform>/ and pack into dist/
#   ./build.sh --clean    remove .build/<platform>/ afterwards (sources stay cached)
#
# Platforms: macos-aarch64 (native), linux-x86_64 and linux-aarch64 (run
# inside ci/linux.sh's Debian bookworm container, or any glibc 2.36 host).
#
# Produces, in dist/:
#   seevue-disc-navigation-<version>-<platform>.tar.xz
#     bin/navd       libdvdnav 7.0.0 and libdvdread linked statically (GPL-2.0-or-later)
#     bin/bdnavd     links lib/libbluray (LGPL-2.1-or-later) dynamically
#     lib/           libbluray and libudfread shared libraries, with FreeType,
#                    fontconfig and expat built into libbluray; the BD-J JARs
#     etc/fonts/     fontconfig's default configuration (FONTCONFIG_FILE)
#     licenses/      every component's licence text, NOTICE and the source offer
#     VERSION, BUILDINFO.json
#   seevue-disc-jre-<jdk version, + as _>-<platform>.tar.xz
#     jre/           jlink image of Temurin JDK 21, modules from jdeps on the BD-J JARs
#   SHA256SUMS-<platform>
#
# Every source comes from sources.lock with its SHA-256 checked. Nothing is
# taken from Homebrew or the system beyond the C compiler, libc and build tools.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
CLEAN=0
[[ "${1:-}" == "--clean" ]] && CLEAN=1

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PLATFORM=macos-aarch64 ;;
  Linux-x86_64) PLATFORM=linux-x86_64 ;;
  Linux-aarch64|Linux-arm64) PLATFORM=linux-aarch64 ;;
  *) echo "unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
OS="${PLATFORM%%-*}"

SOURCES="${SOURCES_DIR:-$ROOT/.build/sources}"
WORK="$ROOT/.build/$PLATFORM"
PREFIX="$WORK/prefix"
SRC="$WORK/src"
DIST="$ROOT/dist"
mkdir -p "$SOURCES" "$SRC" "$PREFIX" "$DIST"
JOBS="${JOBS:-4}"

# --- sources -------------------------------------------------------------
lock() { # name -> "version sha url" for this platform
  awk -v n="$1" -v p="$PLATFORM" '!/^#/ && $1==n && ($3=="any" || $3==p) {print $2, $4, $5; exit}' "$ROOT/sources.lock"
}
fetch() { # name -> path of the verified tarball
  local version sha url file alg=256
  read -r version sha url <<<"$(lock "$1")"
  [[ -n "$url" ]] || { echo "sources.lock has no $1 for $PLATFORM" >&2; exit 1; }
  file="$SOURCES/$(basename "${url//%2B/+}")"
  [[ ${#sha} -eq 128 ]] && alg=512
  if [[ ! -f "$file" ]]; then
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  if ! echo "$sha  $file" | shasum -a "$alg" -c - >/dev/null 2>&1; then
    echo "checksum mismatch: $file (pinned $sha); deleted" >&2
    rm -f "$file"
    exit 1
  fi
  echo "$file"
}
version_of() { lock "$1" | cut -d' ' -f1; }
unpack() { # name -> source directory
  local tarball dir
  tarball="$(fetch "$1")"
  dir="$SRC/$1"
  if [[ ! -d "$dir" ]]; then
    mkdir -p "$dir"
    tar xf "$tarball" -C "$dir" --strip-components=1
  fi
  echo "$dir"
}

# libdvdnav is pinned to 7.0.0: navd reads VM registers through its internal
# headers, which carry no compatibility promise.
[[ "$(version_of libdvdnav)" == "7.0.0" ]] || { echo "navd needs libdvdnav 7.0.0, sources.lock pins $(version_of libdvdnav)" >&2; exit 1; }

# --- toolchain -----------------------------------------------------------
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
NATIVE="$WORK/native.ini"
cat > "$NATIVE" <<EOF
[binaries]
pkg-config = ['pkg-config', '--static']
EOF
if [[ "$OS" == macos ]]; then
  export MACOSX_DEPLOYMENT_TARGET=15.0
  export CFLAGS="-O2 -mmacosx-version-min=15.0"
  export LDFLAGS="-mmacosx-version-min=15.0"
else
  export CFLAGS="-O2 -fPIC"
  export LDFLAGS=""
fi
meson_build() { # name srcdir [meson options...]
  local name="$1" dir="$2"; shift 2
  meson setup --wipe "$WORK/build-$name" "$dir" --prefix "$PREFIX" --libdir lib \
    --buildtype release --native-file "$NATIVE" "$@" >"$WORK/build-$name.log" 2>&1 \
    || { tail -40 "$WORK/build-$name.log" >&2; exit 1; }
  ninja -C "$WORK/build-$name" -j "$JOBS" install >>"$WORK/build-$name.log" 2>&1 \
    || { tail -40 "$WORK/build-$name.log" >&2; exit 1; }
}

# JDK: builds the BD-J JARs and provides jdeps and jlink.
JDK_DIR="$(unpack temurin-jdk)"
JDK_HOME="$JDK_DIR"
[[ -d "$JDK_DIR/Contents/Home" ]] && JDK_HOME="$JDK_DIR/Contents/Home"
ANT_DIR="$(unpack ant)"

# --- libraries -------------------------------------------------------------
stamp() { [[ -f "$WORK/.done-$1" ]]; }
done_() { touch "$WORK/.done-$1"; }

if ! stamp expat; then
  d="$(unpack expat)"
  (cd "$d" && ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-shared --enable-static \
     --with-pic --without-docbook --without-examples --without-tests --without-xmlwf >"$WORK/build-expat.log" 2>&1 \
   && make -j "$JOBS" install >>"$WORK/build-expat.log" 2>&1) || { tail -40 "$WORK/build-expat.log" >&2; exit 1; }
  done_ expat
fi
if ! stamp freetype; then
  meson_build freetype "$(unpack freetype)" -Ddefault_library=static -Dzlib=disabled -Dbzip2=disabled \
    -Dpng=disabled -Dharfbuzz=disabled -Dbrotli=disabled -Dtests=disabled
  done_ freetype
fi
if ! stamp fontconfig; then
  # sysconfdir is the runtime default (/etc/fonts on Linux). The archive also
  # carries etc/fonts/fonts.conf for FONTCONFIG_FILE, so it installs to a
  # staging root first and only the library and headers go into the prefix.
  meson setup --wipe "$WORK/build-fontconfig" "$(unpack fontconfig)" --prefix "$PREFIX" --libdir lib \
    --sysconfdir /etc --localstatedir /var --buildtype release --native-file "$NATIVE" \
    -Ddefault_library=static -Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dcache-build=disabled \
    -Dnls=disabled -Dxml-backend=expat >"$WORK/build-fontconfig.log" 2>&1 \
    || { tail -40 "$WORK/build-fontconfig.log" >&2; exit 1; }
  rm -rf "$WORK/fcstage"
  DESTDIR="$WORK/fcstage" ninja -C "$WORK/build-fontconfig" -j "$JOBS" install >>"$WORK/build-fontconfig.log" 2>&1 \
    || { tail -40 "$WORK/build-fontconfig.log" >&2; exit 1; }
  cp -R "$WORK/fcstage$PREFIX/." "$PREFIX/"
  done_ fontconfig
fi
if ! stamp udfread; then
  meson_build udfread "$(unpack libudfread)" -Ddefault_library=shared
  done_ udfread
fi
if ! stamp bluray; then
  d="$(unpack libbluray)"
  for pt in "$ROOT"/patches/libbluray/*.patch; do
    patch -s -p1 -N -d "$d" < "$pt"
  done
  # The static FreeType, fontconfig and expat stay private to libbluray: on
  # Linux their symbols are not exported, so they never interpose on the JVM's
  # own font libraries (macOS's two-level namespace already keeps them apart).
  link_args=()
  [[ "$OS" == linux ]] && link_args=(-Dc_link_args=-Wl,--exclude-libs,ALL)
  PATH="$ANT_DIR/bin:$JDK_HOME/bin:$PATH" JAVA_HOME="$JDK_HOME" \
    meson_build bluray "$d" ${link_args[@]+"${link_args[@]}"} -Ddefault_library=shared -Dbdj_jar=enabled -Djdk_home="$JDK_HOME" \
      -Dembed_udfread=false -Dfontconfig=enabled -Dfreetype=enabled -Dlibxml2=disabled \
      -Denable_tools=false -Denable_docs=false -Denable_examples=false
  done_ bluray
fi
if ! stamp dvdread; then
  d="$(unpack libdvdread)"
  # An information file whose backup differs: use the backup when it is the intact copy.
  for pt in "$ROOT"/patches/libdvdread/*.patch; do
    patch -s -p1 -N -d "$d" < "$pt"
  done
  meson_build dvdread "$d" -Ddefault_library=static -Dlibdvdcss=disabled -Denable_docs=false
  done_ dvdread
fi
if ! stamp dvdnav; then
  d="$(unpack libdvdnav)"
  # dvdnav_open_files: disc folders through the read service (navd); SPRM 14 kept as navd sets it.
  for pt in "$ROOT"/patches/libdvdnav/*.patch; do
    patch -s -p1 -N -d "$d" < "$pt"
  done
  meson_build dvdnav "$d" -Ddefault_library=static -Denable_docs=false
  done_ dvdnav
fi

# --- workers ---------------------------------------------------------------
STAGE="$WORK/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/etc/fonts" "$STAGE/licenses"
WARN=(-std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter)
# The version both workers announce in their protocol hello.
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "VERSION is not x.y.z: $VERSION" >&2; exit 1; }
WARN+=(-DSEEVUE_WORKER_VERSION="\"$VERSION\"")

# navd: the version guard header fails the compile on any libdvdnav but 7.0.0.
# GCC ignores the gcc_struct attribute in libdvdnav's internal headers (on
# non-x86 targets): that warning only, not navd's own.
NAVD_EXTRA=()
[[ "$OS" == linux ]] && NAVD_EXTRA=(-Wno-attributes)
cc "${WARN[@]}" ${NAVD_EXTRA[@]+"${NAVD_EXTRA[@]}"} -include "$ROOT/src/navd_version_check.h" \
  -I"$PREFIX/include" -I"$SRC/libdvdnav/src" -I"$WORK/build-dvdnav" \
  "$ROOT/src/navd.c" "$ROOT/src/readsvc.c" "$PREFIX/lib/libdvdnav.a" "$PREFIX/lib/libdvdread.a" \
  $([[ "$OS" == linux ]] && echo -lpthread) -o "$STAGE/bin/navd"

if [[ "$OS" == macos ]]; then
  RPATH=(-Wl,-rpath,@loader_path/../lib)
else
  RPATH=(-Wl,-rpath,'$ORIGIN/../lib' -Wl,--enable-new-dtags)
fi
# bdnavd.c sets no feature-test macro; glibc hides nftw, setenv and
# clock_gettime under -std=c11 without one (macOS shows them by default).
BDNAVD_EXTRA=()
[[ "$OS" == linux ]] && BDNAVD_EXTRA=(-D_GNU_SOURCE)
cc "${WARN[@]}" ${BDNAVD_EXTRA[@]+"${BDNAVD_EXTRA[@]}"} -I"$PREFIX/include" "$ROOT/src/bdnavd.c" "$ROOT/src/readsvc.c" "$ROOT/src/sandbox.c" -L"$PREFIX/lib" -lbluray -lpthread \
  "${RPATH[@]}" -o "$STAGE/bin/bdnavd"

# Shared libraries under their sonames, found through the rpath.
if [[ "$OS" == macos ]]; then
  for lib in bluray udfread; do
    real="$(cd "$PREFIX/lib" && ls lib$lib.*.dylib | grep -E "^lib$lib\.[0-9]+\.dylib$")"
    cp "$PREFIX/lib/$real" "$STAGE/lib/$real"
    chmod u+w "$STAGE/lib/$real"
    install_name_tool -id "@rpath/$real" "$STAGE/lib/$real"
  done
  udf="$(cd "$STAGE/lib" && ls libudfread.*.dylib)"
  bl="$(cd "$STAGE/lib" && ls libbluray.*.dylib)"
  old="$(otool -L "$STAGE/lib/$bl" | awk '/libudfread/ {print $1}')"
  install_name_tool -change "$old" "@rpath/$udf" "$STAGE/lib/$bl"
  install_name_tool -add_rpath @loader_path "$STAGE/lib/$bl"
  old="$(otool -L "$STAGE/bin/bdnavd" | awk '/libbluray/ {print $1}')"
  install_name_tool -change "$old" "@rpath/$bl" "$STAGE/bin/bdnavd"
  strip -x "$STAGE/bin/navd" "$STAGE/bin/bdnavd" "$STAGE/lib/"*.dylib
  for f in "$STAGE/bin/navd" "$STAGE/bin/bdnavd" "$STAGE/lib/"*.dylib; do codesign -s - -f "$f" 2>/dev/null; done
else
  for lib in bluray udfread; do
    soname="$(objdump -p "$PREFIX/lib/lib$lib.so" | awk '/SONAME/ {print $2}')"
    cp -L "$PREFIX/lib/lib$lib.so" "$STAGE/lib/$soname"
  done
  patchelf --set-rpath '$ORIGIN' "$STAGE/lib/"libbluray.so.*
  strip --strip-unneeded "$STAGE/bin/navd" "$STAGE/bin/bdnavd" "$STAGE/lib/"*.so.*
fi
cp "$PREFIX/share/java/"libbluray-*.jar "$STAGE/lib/"
cp "$WORK/fcstage/etc/fonts/fonts.conf" "$STAGE/etc/fonts/fonts.conf"
[[ -d "$WORK/fcstage/etc/fonts/conf.d" ]] && cp -RL "$WORK/fcstage/etc/fonts/conf.d" "$STAGE/etc/fonts/conf.d"

# Nothing may point at the build machine: no prefix, Homebrew or /usr/local paths.
if [[ "$OS" == macos ]]; then
  deps="$(otool -L "$STAGE/bin/"* "$STAGE/lib/"*.dylib)"
else
  deps="$(ldd "$STAGE/bin/"* "$STAGE/lib/"*.so.* 2>&1; readelf -d "$STAGE/bin/"* "$STAGE/lib/"*.so.*)"
fi
if grep -E "$PREFIX|/opt/homebrew|/usr/local|not found" <<<"$deps"; then
  echo "a worker depends on a build-machine path" >&2; exit 1
fi

# Licences: each component's own text, from its source tarball.
L="$STAGE/licenses"
cp "$ROOT/COPYING" "$L/seevue-disc-navigation-GPL-2.0.txt"
cp "$SRC/libdvdnav/COPYING" "$L/libdvdnav-COPYING.txt"
cp "$SRC/libdvdread/COPYING" "$L/libdvdread-COPYING.txt"
cp "$SRC/libbluray/COPYING" "$L/libbluray-COPYING.txt"
cp "$SRC/libudfread/COPYING" "$L/libudfread-COPYING.txt"
cp "$SRC/freetype/LICENSE.TXT" "$L/freetype-LICENSE.txt"
cp "$SRC/freetype/docs/FTL.TXT" "$L/freetype-FTL.txt"
cp "$SRC/fontconfig/COPYING" "$L/fontconfig-COPYING.txt"
cp "$SRC/expat/COPYING" "$L/expat-COPYING.txt"
cp "$ROOT/licenses/NOTICE.txt" "$ROOT/licenses/SOURCE-OFFER.txt" "$L/"
cp -R "$ROOT/patches" "$STAGE/licenses/patches"
echo "$VERSION" > "$STAGE/VERSION"

# --- Java runtime ------------------------------------------------------------
JDK_VERSION="$(version_of temurin-jdk)"
# jdeps also warns about libbluray's split packages (it patches java.base and
# java.desktop at run time); the module list is its last line.
MODULES="$("$JDK_HOME/bin/jdeps" --multi-release 21 --ignore-missing-deps --print-module-deps \
  "$STAGE/lib/"libbluray-*.jar 2>/dev/null | grep -v '^Warning' | tail -1)"
[[ "$MODULES" =~ ^[a-z][a-z0-9.]*(,[a-z][a-z0-9.]*)*$ ]] || { echo "jdeps gave no module list: $MODULES" >&2; exit 1; }
JRE="$WORK/jre-stage/jre"
rm -rf "$WORK/jre-stage"
mkdir -p "$WORK/jre-stage"
"$JDK_HOME/bin/jlink" --add-modules "$MODULES" --strip-debug --no-man-pages --no-header-files \
  --compress=zip-6 --output "$JRE"
JRE_BYTES="$(du -sk "$JRE" | awk '{print $1 * 1024}')"

cc_version="$(cc --version | head -1)"
python3 - "$STAGE/BUILDINFO.json" <<EOF
import json, sys
json.dump({
  "name": "seevue-disc-navigation", "version": "$VERSION", "platform": "$PLATFORM",
  "libdvdnav": "$(version_of libdvdnav)", "libdvdread": "$(version_of libdvdread)",
  "libbluray": "$(version_of libbluray)", "libudfread": "$(version_of libudfread)",
  "freetype": "$(version_of freetype)", "fontconfig": "$(version_of fontconfig)",
  "expat": "$(version_of expat)", "jdk": "$JDK_VERSION", "jre_modules": "$MODULES".split(","),
  "jre_bytes": $JRE_BYTES, "cc": "$cc_version",
}, open(sys.argv[1], "w"), indent=2)
EOF
cp "$STAGE/BUILDINFO.json" "$WORK/jre-stage/jre/seevue-BUILDINFO.json"

# --- pack ---------------------------------------------------------------------
W_ARCHIVE="$DIST/seevue-disc-navigation-$VERSION-$PLATFORM.tar.xz"
# Asset names avoid "+", which GitHub release URLs do not keep.
J_ARCHIVE="$DIST/seevue-disc-jre-${JDK_VERSION//+/_}-$PLATFORM.tar.xz"
python3 "$ROOT/scripts/pack.py" "$STAGE" "$W_ARCHIVE"
python3 "$ROOT/scripts/pack.py" "$WORK/jre-stage" "$J_ARCHIVE"
(cd "$DIST" && shasum -a 256 "$(basename "$W_ARCHIVE")" "$(basename "$J_ARCHIVE")" > "SHA256SUMS-$PLATFORM")
cat "$DIST/SHA256SUMS-$PLATFORM"
ls -l "$W_ARCHIVE" "$J_ARCHIVE"
echo "jre: $JRE_BYTES bytes unpacked, modules $MODULES"

if [[ $CLEAN -eq 1 ]]; then
  rm -rf "$WORK"
fi
