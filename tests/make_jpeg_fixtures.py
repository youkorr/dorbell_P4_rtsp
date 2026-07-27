"""Generate the JPEG fixtures test_mjpeg.cpp reads (needs Pillow)."""
import sys
from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else "."
im = Image.new("RGB", (320, 240))
px = im.load()
for y in range(240):
    for x in range(320):
        px[x, y] = ((x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256)
# subsampling 2 = 4:2:0, 1 = 4:2:2, 0 = 4:4:4 (which RFC 2435 cannot describe).
for name, sub in (("f420.jpg", 2), ("f422.jpg", 1), ("f444.jpg", 0)):
    im.save(f"{out}/{name}", quality=60, subsampling=sub)
print(f"fixtures written to {out}")
