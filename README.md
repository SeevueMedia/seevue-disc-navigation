# seevue-disc-navigation

The DVD and Blu-ray navigation helpers that Seevue's server runs.

## Licence

GPL-2.0-or-later (`COPYING`). libbluray and libudfread are
LGPL-2.1-or-later. See `licenses/NOTICE.txt` for every component.

## Building

```bash
./build.sh               # this host (macOS on Apple silicon)
ci/linux.sh x86_64       # Linux, in Docker
ci/linux.sh aarch64
```

Needs a C compiler, meson, ninja, pkg-config, curl and Python 3 (Linux builds need only Docker).

## Worker sandbox

`bdnavd` applies its policy before its protocol 6 hello. Linux uses Landlock
and a default-deny seccomp filter; macOS checks for `sandbox-exec`. The hello
names the sandbox or reports `unavailable`. Java stays off without one.

The caller supplies `BDNAVD_SANDBOX=1`, newline-separated readable and
writable paths in `BDNAVD_SANDBOX_READ` and `BDNAVD_SANDBOX_WRITE`, and
`BDNAVD_RLIMIT_AS`, `BDNAVD_RLIMIT_FSIZE`, `BDNAVD_RLIMIT_NOFILE`,
`BDNAVD_RLIMIT_CPU` and `BDNAVD_NICE` where supported. `BDNAVD_JAVA_OPTIONS`
sets JVM bounds. Cache and temp use `TMPDIR`; `BDNAVD_PERSISTENT` is optional.

`ci/sandbox-test.sh` probes containment with temporary targets.
`ci/control.sh` builds an uncontained test worker and permissive Java test
JAR; neither is packaged.
