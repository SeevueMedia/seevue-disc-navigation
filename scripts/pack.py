#!/usr/bin/env python3
"""Pack a directory into a reproducible .tar.xz: sorted entries, fixed
mtime, owner 0, modes kept (0755 for executables, 0644 otherwise).

usage: pack.py DIRECTORY ARCHIVE.tar.xz
Entries are relative to DIRECTORY (no top-level folder). Symlinks are kept
only when they stay inside DIRECTORY."""
import os
import sys
import tarfile
from pathlib import Path

MTIME = 1_767_225_600  # 2026-01-01T00:00:00Z


def main(src, out):
    src = Path(src).resolve()
    out = Path(out)
    tmp = out.with_suffix(out.suffix + ".part")
    with tarfile.open(tmp, "w:xz", preset=9, format=tarfile.PAX_FORMAT) as tar:
        for path in sorted(src.rglob("*")):
            rel = path.relative_to(src).as_posix()
            info = tar.gettarinfo(str(path), arcname=rel)
            if info.issym():
                target = (path.parent / os.readlink(path)).resolve()
                if not target.is_relative_to(src):
                    raise SystemExit(f"{rel}: symlink leaves the archive")
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = MTIME
            if info.isfile():
                info.mode = 0o755 if info.mode & 0o111 else 0o644
                with open(path, "rb") as f:
                    tar.addfile(info, f)
            elif info.isdir():
                info.mode = 0o755
                tar.addfile(info)
            elif info.issym():
                tar.addfile(info)
            else:
                raise SystemExit(f"{rel}: unsupported file type")
    tmp.rename(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
