# Regenerates the PNG launcher icons (Android 7, API 24-25) in android/app/src/main/res/mipmap-*/:
#   python3 android/tools/make_icon.py android/app/src/main/res
# Android 8+ uses the adaptive icon (mipmap-anydpi-v26, drawable/ic_launcher_*.xml): keep both in step.
import sys
from PIL import Image, ImageDraw

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def gradient(size):
    stops = [(0.0, (0x1D, 0x3A, 0x8A)), (0.55, (0x14, 0x15, 0x28)), (1.0, (0x0E, 0x0D, 0x14))]
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2 * (size - 1))
            for (o0, c0), (o1, c1) in zip(stops, stops[1:]):
                if t <= o1:
                    px[x, y] = lerp(c0, c1, (t - o0) / (o1 - o0))
                    break
    return img

def icon(size, shape, logo_frac):
    s = size * 4  # supersampled
    img = gradient(s).convert("RGBA")
    # Logo: rounded square 44/108 of the adaptive canvas -> logo_frac of this icon.
    L = s * logo_frac
    x0 = (s - L) / 2
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x0, x0, x0 + L, x0 + L], radius=L * 10 / 44, fill=(255, 255, 255, 255))
    k = L / 44  # 108-canvas units per pixel, relative to the square's corner at (32,32)
    tri = [(x0 + (48 - 32) * k, x0 + (42 - 32) * k), (x0 + (48 - 32) * k, x0 + (66 - 32) * k),
           (x0 + (69 - 32) * k, x0 + (54 - 32) * k)]
    d.polygon(tri, fill=(0x0E, 0x0D, 0x14, 255))
    mask = Image.new("L", (s, s), 0)
    md = ImageDraw.Draw(mask)
    if shape == "round":
        md.ellipse([0, 0, s - 1, s - 1], fill=255)
    else:
        md.rounded_rectangle([0, 0, s - 1, s - 1], radius=s * 0.18, fill=255)
    img.putalpha(mask)
    return img.resize((size, size), Image.LANCZOS)

if __name__ == "__main__":
    res = sys.argv[1]
    for name, px in [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]:
        import os
        os.makedirs(f"{res}/mipmap-{name}", exist_ok=True)
        icon(px, "square", 0.56).save(f"{res}/mipmap-{name}/ic_launcher.png")
        icon(px, "round", 0.52).save(f"{res}/mipmap-{name}/ic_launcher_round.png")
    if len(sys.argv) > 2:
        prev = Image.new("RGBA", (3 * 300 + 40, 340), (240, 240, 245, 255))
        prev.alpha_composite(icon(256, "square", 0.56), (22, 42))
        prev.alpha_composite(icon(256, "round", 0.52), (322, 42))
        prev.alpha_composite(icon(256, "round", 0.62), (622, 42))
        prev.save(sys.argv[2])
