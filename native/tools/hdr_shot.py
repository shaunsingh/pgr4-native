"""Luminance distribution of an HDR10 screenshot (gamescopectl screenshot <name>.avif while the
game presents HDR10: deck-run.sh --hdr --shots N), to check what an HDR frame puts on screen.

gamescope writes the AVIF lossless with BT.2020 primaries, the PQ curve (SMPTE ST 2084), full
range and the identity matrix: its 10-bit "YUV 4:4:4" planes are G, B and R (steamcompmgr.cpp's
screenshot writer). ffmpeg reads the planes as they are; each component goes through the PQ EOTF
to nits, and the luminance is BT.2020's weighting of the three. Prints the percentiles of the luminance and the share of pixels above each threshold,
so a frame whose highlights stop at SDR white (--pgr4_native_hdr_paper_white) is told apart from
one that uses the headroom.

usage: hdr_shot.py <shot.avif> [thresholds in nits, default 100 203 400 600 1000]
"""
import array
import subprocess
import sys

M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq_to_nits(e):
    p = e ** (1 / M2)
    return 10000.0 * (max(p - C1, 0.0) / (C2 - C3 * p)) ** (1 / M1)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    thresholds = [float(t) for t in sys.argv[2:]] or [100, 203, 400, 600, 1000]
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,pix_fmt", "-of", "csv=p=0", path],
        check=True, capture_output=True, text=True).stdout.strip().splitlines()[0].split(",")
    width, height, pix_fmt = int(probe[0]), int(probe[1]), probe[2]
    if pix_fmt != "yuv444p10le":
        sys.exit(f"{path}: {pix_fmt}, not gamescope's 10-bit 4:4:4 HDR screenshot")
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt",
         pix_fmt, "-"], check=True, capture_output=True).stdout
    n = width * height
    planes = array.array("H")
    planes.frombytes(raw[: n * 6])
    g, b, r = planes[:n], planes[n : 2 * n], planes[2 * n :]
    # The PQ EOTF of each 10-bit code, once.
    table = [pq_to_nits(i / 1023.0) for i in range(1024)]
    lum = sorted(0.2627 * table[r[i]] + 0.6780 * table[g[i]] + 0.0593 * table[b[i]]
                 for i in range(n))
    print(f"{path}: {width}x{height}")
    print("luminance nits: " + ", ".join(
        f"p{q} {lum[min(n - 1, int(n * q / 100))]:.0f}" for q in (50, 90, 99, 99.9)) +
        f", max {lum[-1]:.0f}")
    print("pixels above: " + ", ".join(
        f"{t:.0f} nits {sum(1 for v in lum if v > t) * 100.0 / n:.2f} %" for t in thresholds))


if __name__ == "__main__":
    main()
