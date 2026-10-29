"""Resolve freedesktop desktop icons and publish native launcher assets.

The .desktop file is the source of truth.  This module does not know any
application names and never substitutes another application's artwork.
"""
from __future__ import annotations

import base64
import configparser
import io
import json
import os
from pathlib import Path
import re
import subprocess
from typing import Sequence

from PIL import Image


ICON_EXTENSIONS = (".png", ".svg", ".xpm")
BUNDLE_RE = re.compile(r"org\.l410\.linux\.[a-z][a-z0-9_.]*")


def _inside(root: Path, path: Path) -> Path | None:
    try:
        resolved = path.resolve(strict=True)
        return resolved if resolved.is_relative_to(root.resolve(strict=True)) else None
    except (OSError, RuntimeError):
        return None


def _directory_size(name: str, section: configparser.SectionProxy | None) -> int:
    if section is not None:
        try:
            return max(1, section.getint("Size"))
        except (ValueError, configparser.Error):
            pass
    match = re.match(r"^(\d+)x(\d+)$", Path(name).parts[0] if Path(name).parts else "")
    return int(match.group(1)) if match else 64


def _theme_directories(theme: Path, preferred_size: int):
    index = theme / "index.theme"
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    directories: list[str] = []
    if index.is_file():
        try:
            parser.read(index, encoding="utf-8")
            directories = [part.strip() for part in
                           parser.get("Icon Theme", "Directories", fallback="").split(",")
                           if part.strip()]
        except (OSError, configparser.Error):
            directories = []
    if not directories:
        try:
            directories = [str(path.relative_to(theme)) for path in theme.glob("*/apps")
                           if path.is_dir()]
            directories += [str(path.relative_to(theme)) for path in theme.glob("scalable/apps")
                            if path.is_dir()]
        except OSError:
            pass
    scored = []
    for order, directory in enumerate(dict.fromkeys(directories)):
        if Path(directory).is_absolute() or ".." in Path(directory).parts:
            continue
        section = parser[directory] if parser.has_section(directory) else None
        size = _directory_size(directory, section)
        kind = section.get("Type", "Threshold") if section is not None else "Threshold"
        # Prefer exact/nearby fixed icons, then scalable artwork.  A larger
        # raster is preferred over an equally distant smaller raster.
        distance = abs(size - preferred_size)
        larger_penalty = 0 if size >= preferred_size else 1
        scalable_penalty = 1 if kind == "Scalable" else 0
        scored.append(((distance, larger_penalty, scalable_penalty, order), theme / directory))
    return [path for _score, path in sorted(scored)]


def resolve_icon(root: Path | str, icon: str, preferred_size: int = 64) -> Path | None:
    """Resolve one Icon= value inside *root* without escaping the rootfs."""
    root = Path(root)
    if not icon or "\0" in icon or len(icon) > 512:
        return None
    raw = Path(icon)
    if raw.is_absolute():
        candidate = _inside(root, root / str(raw).lstrip("/"))
        return candidate if candidate and candidate.is_file() and candidate.suffix.lower() in ICON_EXTENSIONS else None
    if len(raw.parts) != 1 or raw.name in (".", ".."):
        return None
    names = [raw.name] if raw.suffix.lower() in ICON_EXTENSIONS else [raw.name + ext for ext in ICON_EXTENSIONS]
    homes = [root / "home/linux/.icons", root / "home/linux/.local/share/icons"]
    system = [root / "usr/local/share/icons", root / "usr/share/icons"]
    themes = []
    for base in homes + system:
        if not base.is_dir():
            continue
        # hicolor is the required fallback theme and is deterministic when no
        # per-user GTK/Qt theme is active in the management service.
        ordered = ([base / "hicolor"] if (base / "hicolor").is_dir() else [])
        try:
            ordered += sorted((path for path in base.iterdir()
                               if path.is_dir() and path.name != "hicolor"), key=lambda p: p.name)
        except OSError:
            pass
        themes += ordered
    for theme in themes:
        for directory in _theme_directories(theme, preferred_size):
            for name in names:
                candidate = _inside(root, directory / name)
                if candidate and candidate.is_file() and candidate.suffix.lower() in ICON_EXTENSIONS:
                    return candidate
    for base in (root / "usr/local/share/pixmaps", root / "usr/share/pixmaps"):
        for name in names:
            candidate = _inside(root, base / name)
            if candidate and candidate.is_file() and candidate.suffix.lower() in ICON_EXTENSIONS:
                return candidate
    return None


def icon_asset(root: Path | str, icon: str) -> dict | None:
    path = resolve_icon(root, icon)
    if path is None or path.stat().st_size > 2 * 1024 * 1024:
        return None
    data = path.read_bytes()
    return {
        "extension": path.suffix.lower(),
        "data": base64.b64encode(data).decode("ascii"),
        "stamp": [str(path.relative_to(Path(root).resolve())), len(data), path.stat().st_mtime_ns],
    }


def _rgba(source: Path, size: int, svg_converter: str | Sequence[str]) -> Image.Image:
    if source.suffix.lower() == ".svg":
        command = [svg_converter] if isinstance(svg_converter, str) else list(svg_converter)
        result = subprocess.run(
            command + ["--keep-aspect-ratio", "--width", str(size),
                       "--height", str(size), str(source)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
        if result.returncode:
            raise RuntimeError("SVG 图标转换失败: " + result.stderr.decode("utf-8", "replace")[-500:])
        image = Image.open(io.BytesIO(result.stdout))
    else:
        image = Image.open(source)
    image.load()
    image = image.convert("RGBA")
    image.thumbnail((size, size), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(image, ((size - image.width) // 2, (size - image.height) // 2))
    return canvas


def bgra_bytes(source: Path, size: int = 64,
               svg_converter: str | Sequence[str] = "/usr/bin/rsvg-convert") -> bytes:
    rgba = _rgba(source, size, svg_converter).tobytes()
    bgra = bytearray(len(rgba))
    bgra[0::4] = rgba[2::4]
    bgra[1::4] = rgba[1::4]
    bgra[2::4] = rgba[0::4]
    bgra[3::4] = rgba[3::4]
    return bytes(bgra)


def png_bytes(source: Path, size: int = 64,
              svg_converter: str | Sequence[str] = "/usr/bin/rsvg-convert") -> bytes:
    output = io.BytesIO()
    _rgba(source, size, svg_converter).save(output, format="PNG", optimize=True)
    return output.getvalue()


def _write_changed(path: Path, data: bytes) -> bool:
    try:
        if path.is_file() and path.read_bytes() == data:
            return False
    except OSError:
        pass
    temporary = path.with_name(path.name + ".new")
    temporary.write_bytes(data)
    os.chmod(temporary, 0o644)
    os.replace(temporary, path)
    return True


def sync_anco_icons(root: Path | str, catalog: Path | str, title_dir: Path | str,
                    svg_converter: str | Sequence[str] = "/usr/bin/rsvg-convert") -> dict:
    """Materialize exactly the title icons owned by one active AnCo catalog."""
    root, catalog, title_dir = Path(root), Path(catalog), Path(title_dir)
    records = json.loads((catalog / "packages.json").read_text(encoding="utf-8"))
    icon_dir = title_dir / "icons"
    icon_dir.mkdir(parents=True, exist_ok=True)
    previous_file = icon_dir / ".desktop-icons.json"
    try:
        previous = set(json.loads(previous_file.read_text(encoding="utf-8")).get("bundles", []))
    except (OSError, ValueError, TypeError):
        previous = set()
    published: dict[str, dict] = {}
    errors: dict[str, str] = {}
    for bundle, record in sorted(records.items()):
        if not BUNDLE_RE.fullmatch(bundle) or record.get("bundle") != bundle:
            errors[bundle] = "catalog bundle invalid"
            continue
        desktop_name = record.get("desktop", "")
        if Path(desktop_name).name != desktop_name or not desktop_name.endswith(".desktop"):
            errors[bundle] = "desktop id invalid"
            continue
        desktop = root / "usr/share/applications" / desktop_name
        try:
            parser = configparser.ConfigParser(interpolation=None, strict=False)
            parser.read(desktop, encoding="utf-8")
            icon_name = parser["Desktop Entry"].get("Icon", "")
            source = resolve_icon(root, icon_name)
            if source is None:
                raise ValueError("Icon= resource not found")
            data = bgra_bytes(source, 64, svg_converter)
            png = png_bytes(source, 64, svg_converter)
            if len(data) != 64 * 64 * 4:
                raise ValueError("unexpected BGRA size")
            if not png.startswith(b"\x89PNG\r\n\x1a\n"):
                raise ValueError("unexpected PNG output")
            changed = _write_changed(icon_dir / (bundle + ".bgra"), data)
            changed = _write_changed(icon_dir / (bundle + ".png"), png) or changed
            published[bundle] = {"desktop": desktop_name,
                                 "source": str(source.relative_to(root.resolve())),
                                 "changed": changed}
        except (OSError, ValueError, KeyError, configparser.Error, RuntimeError) as error:
            errors[bundle] = str(error)
    owned = set(published)
    for bundle in previous - owned:
        if BUNDLE_RE.fullmatch(bundle):
            (icon_dir / (bundle + ".bgra")).unlink(missing_ok=True)
            (icon_dir / (bundle + ".png")).unlink(missing_ok=True)
    manifest = json.dumps({"version": 2, "bundles": sorted(owned),
                           "published": published, "errors": errors},
                          ensure_ascii=False, indent=2).encode("utf-8")
    _write_changed(previous_file, manifest)
    return {"published": published, "errors": errors}
