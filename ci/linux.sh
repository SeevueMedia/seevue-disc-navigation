#!/usr/bin/env bash
# Builds the Linux archives inside Debian bookworm (glibc 2.36), so the
# workers run there and on newer systems.
#
#   ci/linux.sh x86_64|aarch64     on any Docker host (uses emulation if needed)
#   ci/linux.sh --inside           inside the container (CI runs this directly)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# debian:bookworm-slim multi-platform index.
IMAGE=debian:bookworm-slim@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3

if [[ "${1:-}" == "--inside" ]]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends build-essential pkg-config ninja-build python3 python3-venv \
    curl ca-certificates xz-utils patchelf file perl gperf >/dev/null
  python3 -m venv /opt/buildtools
  /opt/buildtools/bin/pip install -q --require-hashes -r "$ROOT/ci/requirements.txt"
  export PATH="/opt/buildtools/bin:$PATH"
  exec "$ROOT/build.sh" "${@:2}"
fi
case "${1:-}" in
  x86_64) docker_platform=linux/amd64 ;;
  aarch64) docker_platform=linux/arm64 ;;
  *) echo "usage: ci/linux.sh x86_64|aarch64 [--clean]" >&2; exit 2 ;;
esac
exec docker run --rm --platform "$docker_platform" -v "$ROOT:/src" -w /src "$IMAGE" ci/linux.sh --inside "${@:2}"
