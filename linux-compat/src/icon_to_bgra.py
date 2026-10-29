#!/usr/bin/env python3
"""Convert one desktop icon to the fixed PixelMap BGRA input used by AnCo."""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--size", type=int, default=64)
    args = parser.parse_args()
    if not 16 <= args.size <= 512:
        raise SystemExit("size must be between 16 and 512")

    with Image.open(args.source) as image:
        image = image.convert("RGBA")
        image.thumbnail((args.size, args.size), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (args.size, args.size), (0, 0, 0, 0))
        canvas.alpha_composite(image, ((args.size - image.width) // 2,
                                       (args.size - image.height) // 2))
        rgba = canvas.tobytes()

    bgra = bytearray(len(rgba))
    bgra[0::4] = rgba[2::4]
    bgra[1::4] = rgba[1::4]
    bgra[2::4] = rgba[0::4]
    bgra[3::4] = rgba[3::4]
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(bgra)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
