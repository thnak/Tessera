# SPDX-License-Identifier: MIT
"""Regenerates the WebP fixtures (needs Pillow). The outputs are committed; tests do not run this."""
import pathlib
from PIL import Image

out = pathlib.Path(__file__).resolve().parent

# photo.webp: lossy 512x384 gradient with a diagonal edge.
photo = Image.new("RGB", (512, 384))
px = photo.load()
for y in range(384):
    for x in range(512):
        px[x, y] = (x * 255 // 512, y * 255 // 384, 220 if x * 384 > y * 512 else 40)
photo.save(out / "photo.webp", "WEBP", quality=85, method=4)

# alpha.webp: lossless 64x48, left half transparent, right half opaque green.
alpha = Image.new("RGBA", (64, 48), (0, 0, 0, 0))
apx = alpha.load()
for y in range(48):
    for x in range(32, 64):
        apx[x, y] = (0, 200, 0, 255)
alpha.save(out / "alpha.webp", "WEBP", lossless=True)

# anim.webp: two frames, red then blue. Only the first is used.
frames = [Image.new("RGB", (64, 48), c) for c in ((220, 0, 0), (0, 0, 220))]
frames[0].save(out / "anim.webp", "WEBP", save_all=True, append_images=frames[1:],
               duration=100, lossless=True, loop=0)
