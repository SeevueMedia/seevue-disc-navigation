#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/work"
printf canary > "$tmp/canary"
extra=()
[[ "$(uname -s)" == Linux ]] && extra=(-D_GNU_SOURCE)
cc ${extra[@]+"${extra[@]}"} -std=c11 -O2 -Wall -Wextra -Werror -I"$root/src" \
  "$root/ci/sandbox-test.c" "$root/src/sandbox.c" -pthread -o "$tmp/probe"
export BDNAVD_SANDBOX=1 BDNAVD_SANDBOX_WRITE="$tmp/work" BDNAVD_RLIMIT_NOFILE=64
export BDNAVD_RLIMIT_CPU=10 BDNAVD_RLIMIT_FSIZE=8 BDNAVD_NICE=10
if [[ "$(uname -s)" == Linux ]]; then
  export BDNAVD_RLIMIT_AS=536870912
  (cd "$tmp/work" && "$tmp/probe" contained "$tmp/canary")
  (cd "$tmp/work" && "$tmp/probe" missing "$tmp/canary")
else
  tmp="$(cd "$tmp" && pwd -P)"
  cat > "$tmp/profile.sb" <<PROFILE
(version 1)
(deny default)
(import "dyld-support.sb")
(allow process-exec (literal "$tmp/probe"))
(allow file-read* file-map-executable (subpath "/System/Library") (subpath "/usr/lib") (literal "$tmp/probe"))
(allow file-read-metadata)
(allow file-read* file-write* (subpath "$tmp/work"))
(allow sysctl-read)
(allow system-sched (target self))
(allow signal (target self))
PROFILE
  python3 - "$tmp" <<'PYTEST'
import os, socket, subprocess, sys
root = sys.argv[1]
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0)); listener.listen(1)
    env = dict(os.environ, TEST_PORT=str(listener.getsockname()[1]))
    subprocess.run(['/usr/bin/sandbox-exec', '-f', root+'/profile.sb', root+'/probe', 'contained', root+'/canary'], cwd=root+'/work', env=env, check=True)
    listener.setblocking(False)
    try:
        client, _ = listener.accept()
    except BlockingIOError:
        pass
    else:
        client.close()
        raise AssertionError('sandbox reached listener')
PYTEST
fi
