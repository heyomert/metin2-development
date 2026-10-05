"""Mean gradient magnitude (sharpness) of ground regions in two lossless screenshots.

Usage: python sharpness.py before.png after.png
Regions avoid the UI (quest list, inventory, minimap, taskbar) and the character at screen centre; they are
in screen space, so the two screenshots should show the same place from a similar camera angle. A blurrier
texture gives a lower value; anisotropic filtering raises it most in the distant (upper) band.
"""
import sys
from PIL import Image

# (name, left, top, right, bottom) for a 1360x768 frame
REGIONS = [
    ("far ground (rows 150-300)", 300, 150, 1180, 300),
    ("mid ground (rows 300-450, no character)", 760, 300, 1180, 450),
    ("near tiles (rows 470-720)", 280, 470, 1180, 720),
]


def sharpness(img, box):
    g = img.convert("L").crop(box)
    w, h = g.size
    px = g.load()
    total = 0
    for y in range(h - 1):
        for x in range(w - 1):
            v = px[x, y]
            total += abs(px[x + 1, y] - v) + abs(px[x, y + 1] - v)
    return total / ((w - 1) * (h - 1))


def main():
    before, after = Image.open(sys.argv[1]), Image.open(sys.argv[2])
    print(f"sizes: before {before.size}, after {after.size}")
    for name, *box in REGIONS:
        b, a = sharpness(before, box), sharpness(after, box)
        print(f"{name:42s} before {b:6.2f}  after {a:6.2f}  change {100.0 * (a - b) / b:+6.1f}%")


if __name__ == "__main__":
    main()
