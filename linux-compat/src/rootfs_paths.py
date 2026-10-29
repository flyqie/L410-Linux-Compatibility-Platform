"""Resolve guest paths with chroot semantics from outside the rootfs.

``pathlib`` follows an absolute symlink against the host root.  Debian packages
quite normally install links such as ``/usr/bin/app -> /opt/app/app``; while the
management service is outside the chroot that target must be rebased below the
configured rootfs.  This module performs only that path translation and does
not execute anything.
"""
import os
import stat
from pathlib import Path, PurePosixPath


def resolve_rootfs_path(root, guest_path, max_links=40):
    """Return the host Path for an existing absolute guest path, or None."""
    if not isinstance(guest_path, str) or not guest_path.startswith('/'):
        return None
    root = Path(root or '/')
    pending = list(PurePosixPath(guest_path).parts[1:])
    resolved = []
    links = 0
    while pending:
        part = pending.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            if not resolved:
                return None
            resolved.pop()
            continue
        candidate = root.joinpath(*resolved, part)
        try:
            mode = candidate.lstat().st_mode
        except OSError:
            return None
        if stat.S_ISLNK(mode):
            links += 1
            if links > max_links:
                return None
            try:
                target = PurePosixPath(os.readlink(candidate))
            except OSError:
                return None
            target_parts = list(target.parts)
            if target.is_absolute():
                resolved = []
                target_parts = target_parts[1:]
            pending = target_parts + pending
        else:
            resolved.append(part)
    return root.joinpath(*resolved)


def rootfs_regular_file(root, guest_path):
    path = resolve_rootfs_path(root, guest_path)
    try:
        return path if path is not None and path.is_file() else None
    except OSError:
        return None


def rootfs_executable(root, guest_path):
    path = rootfs_regular_file(root, guest_path)
    try:
        return path if path is not None and os.access(path, os.X_OK) else None
    except OSError:
        return None
