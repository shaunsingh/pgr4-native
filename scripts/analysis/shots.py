#!/usr/bin/env python3
"""Per-shot numbers for a run's screenshots (deck-run.sh --shots), and a contact sheet of them.

For each PNG, in name order: how much it changed from the one before (mean absolute difference,
0-255), the sharpness of --region (mean absolute difference of horizontal neighbours: an upscaler
or a blur that softens the image lowers it) and, for each --color, how many pixels are within
--tolerance of it in every channel. A diagnostic build that paints what it is looking for in a
colour no scene shows (pure magenta, say) then gives a count per shot.

  shots.py <dir | png ...> [--skip N] [--region x0,y0,x1,y1] [--color R,G,B ...] [--tolerance 75]
           [--sheet OUT.png] [--cols 2]

--skip N leaves out the first N shots (the boot and menus of a route). --sheet writes the shots
side by side, --cols per row, at full size. Needs numpy and Pillow.
"""

import argparse
import glob
import os

import numpy as np
from PIL import Image


def triple(s):
    v = tuple(int(x) for x in s.split(","))
    if len(v) != 3:
        raise argparse.ArgumentTypeError("want R,G,B")
    return v


def box(s):
    v = tuple(int(x) for x in s.split(","))
    if len(v) != 4:
        raise argparse.ArgumentTypeError("want x0,y0,x1,y1")
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--region", type=box)
    ap.add_argument("--color", type=triple, nargs="*", default=[])
    ap.add_argument("--tolerance", type=int, default=75)
    ap.add_argument("--sheet")
    ap.add_argument("--cols", type=int, default=2)
    args = ap.parse_args()

    files = []
    for p in args.paths:
        files += sorted(glob.glob(os.path.join(p, "*.png"))) if os.path.isdir(p) else [p]
    files = files[args.skip:]
    if not files:
        raise SystemExit("no shots")

    prev, images = None, []
    for f in files:
        a = np.asarray(Image.open(f).convert("RGB"), dtype=np.int16)
        x0, y0, x1, y1 = args.region or (0, 0, a.shape[1], a.shape[0])
        roi = a[y0:y1, x0:x1]
        sharp = np.abs(np.diff(roi, axis=1)).mean()
        diff = np.abs(a - prev).mean() if prev is not None and prev.shape == a.shape else 0.0
        counts = [int((np.abs(a - np.array(c)) <= args.tolerance).all(axis=2).sum())
                  for c in args.color]
        cols = "".join(f"  {c[0]},{c[1]},{c[2]}: {n}" for c, n in zip(args.color, counts))
        print(f"{os.path.basename(f)}  diff {diff:6.2f}  sharp {sharp:6.2f}{cols}")
        prev = a
        if args.sheet:
            images.append(a.astype(np.uint8))

    if args.sheet:
        h, w = images[0].shape[:2]
        rows = (len(images) + args.cols - 1) // args.cols
        sheet = np.zeros((rows * h, args.cols * w, 3), dtype=np.uint8)
        for i, im in enumerate(images):
            if im.shape[:2] == (h, w):
                r, c = divmod(i, args.cols)
                sheet[r * h:(r + 1) * h, c * w:(c + 1) * w] = im
        Image.fromarray(sheet).save(args.sheet)
        print(f"sheet: {args.sheet} ({len(images)} shots)")


if __name__ == "__main__":
    main()
