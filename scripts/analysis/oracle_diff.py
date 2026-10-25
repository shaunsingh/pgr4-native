"""Where native and Xenos differ in oracle captures: per-frame composite PNGs and a tile report.

For every f<frame>-native.png / f<frame>-xenos.png pair in a capture directory (written by the
renderer's capture flow, --pgr4_native_capture_dir), writes to <out>:
  f<frame>-diff.png   native | xenos | |native - xenos| x gain (heat colours), at half size
  f<frame>-worst.png  native and xenos crops of the worst tile region, at full size, side by side
  summary.txt         PSNR per frame, then the worst tiles (PSNR per tile, pixel rectangle)
and prints the summary. A tile's PSNR locates what differs: one wrong draw shows up as a few
low tiles, a global shift (tonemap, gamma) as all tiles low.

usage: oracle_diff.py <capture dir> [--out DIR] [--tiles 16x9] [--gain 8] [--worst 8]
needs: numpy, Pillow
"""
import argparse
import math
import pathlib
import re

import numpy as np
from PIL import Image, ImageDraw


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2))
    return 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def heat(d: np.ndarray, gain: float) -> np.ndarray:
    """Mean absolute RGB difference x gain as black-red-yellow-white."""
    v = np.clip(d.mean(axis=2) * gain / 255.0, 0, 1)
    r = np.clip(v * 3, 0, 1)
    g = np.clip(v * 3 - 1, 0, 1)
    b = np.clip(v * 3 - 2, 0, 1)
    return (np.stack([r, g, b], axis=2) * 255).astype(np.uint8)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--out")
    ap.add_argument("--tiles", default="16x9")
    ap.add_argument("--gain", type=float, default=8.0)
    ap.add_argument("--worst", type=int, default=8)
    args = ap.parse_args()
    src = pathlib.Path(args.dir)
    out = pathlib.Path(args.out) if args.out else src / "diff"
    out.mkdir(parents=True, exist_ok=True)
    tx, ty = (int(v) for v in args.tiles.lower().split("x"))
    lines = []
    frames = sorted(int(m.group(1)) for p in src.glob("f*-native.png") if (m := re.match(r"f(\d+)-native", p.stem)))
    for f in frames:
        npath, xpath = src / f"f{f}-native.png", src / f"f{f}-xenos.png"
        if not xpath.exists():
            continue
        n = np.asarray(Image.open(npath).convert("RGB"))
        x = np.asarray(Image.open(xpath).convert("RGB"))
        h, w, _ = n.shape
        d = np.abs(n.astype(np.int16) - x.astype(np.int16)).astype(np.uint8)
        total = psnr(n, x)
        th, tw = h // ty, w // tx
        tiles = []
        for j in range(ty):
            for i in range(tx):
                ys, xs = slice(j * th, (j + 1) * th), slice(i * tw, (i + 1) * tw)
                tiles.append((psnr(n[ys, xs], x[ys, xs]), i, j))
        tiles.sort()
        # Composite: the three panels side by side at half size, worst tiles outlined in cyan.
        panels = [Image.fromarray(n), Image.fromarray(x), Image.fromarray(heat(d, args.gain))]
        comp = Image.new("RGB", (w * 3, h))
        for k, p in enumerate(panels):
            comp.paste(p, (k * w, 0))
        draw = ImageDraw.Draw(comp)
        for _, i, j in tiles[: args.worst]:
            for k in range(3):
                draw.rectangle([k * w + i * tw, j * th, k * w + (i + 1) * tw - 1, (j + 1) * th - 1], outline=(0, 255, 255), width=2)
        draw.text((8, 8), f"native  f{f}  {total:.2f} dB", fill=(255, 255, 0))
        draw.text((w + 8, 8), "xenos", fill=(255, 255, 0))
        draw.text((2 * w + 8, 8), f"|diff| x{args.gain:g}", fill=(255, 255, 0))
        comp.resize((w * 3 // 2, h // 2), Image.LANCZOS).save(out / f"f{f}-diff.png")
        # Full-size crops around the worst tile (3x3 tiles), native left, xenos right.
        _, wi, wj = tiles[0]
        x0, y0 = max(0, (wi - 1) * tw), max(0, (wj - 1) * th)
        x1, y1 = min(w, (wi + 2) * tw), min(h, (wj + 2) * th)
        crop = Image.new("RGB", ((x1 - x0) * 2 + 8, y1 - y0), (40, 40, 40))
        crop.paste(Image.fromarray(n[y0:y1, x0:x1]), (0, 0))
        crop.paste(Image.fromarray(x[y0:y1, x0:x1]), (x1 - x0 + 8, 0))
        crop.save(out / f"f{f}-worst.png")
        lines.append(f"f{f}: {total:.2f} dB; worst tiles ({tw}x{th} px):")
        for p, i, j in tiles[: args.worst]:
            lines.append(f"  tile ({i},{j}) px [{i * tw},{j * th}]-[{(i + 1) * tw},{(j + 1) * th}]: {p:.1f} dB")
        good = sum(1 for p, _, _ in tiles if p >= 30)
        lines.append(f"  {good}/{len(tiles)} tiles at >= 30 dB; median tile {sorted(t[0] for t in tiles)[len(tiles) // 2]:.1f} dB")
        # Mean |difference| per 40-row band: a step between bands points at screen-space state
        # (tiling, scissor, viewport) rather than at one draw.
        rows = d.mean(axis=(1, 2))
        lines.append("  mean |diff| per 40-row band: " + " ".join(f"{rows[k:k + 40].mean():.1f}" for k in range(0, h, 40)))
    text = "\n".join(lines) if lines else f"no f*-native.png / f*-xenos.png pairs in {src}"
    (out / "summary.txt").write_text(text + "\n")
    print(text)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
