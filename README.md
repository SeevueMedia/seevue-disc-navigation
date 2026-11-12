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
