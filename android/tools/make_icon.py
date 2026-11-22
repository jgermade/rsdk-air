# Regenerates rsdk-air's Android launcher icons from android/icon/rsdk-air-icon.jpg (the
# owner's artwork, 2048 x 2048: a picture inside a navy frame, a rounded square on white
# corners):
#   python3 android/tools/make_icon.py android/app/src/main/res
#
# - Android 8+ (adaptive icon, mipmap-anydpi-v26): the picture without its frame is the
#   foreground layer, 84 of the 108 dp canvas (launchers show about 72 dp through their
#   mask, so the edges of the picture may be trimmed, the middle never);
#   drawable-*dpi/ic_launcher_foreground.png.
# - Android 7 (legacy): the whole artwork with its frame (white corners made transparent),
#   and a round one cut from the picture; mipmap-*dpi/ic_launcher{,_round}.png.
import os
import sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "icon", "rsdk-air-icon.jpg")
DENSITIES = [("mdpi", 1.0), ("hdpi", 1.5), ("xhdpi", 2.0), ("xxhdpi", 3.0), ("xxxhdpi", 4.0)]
CORNER = 0.173  # radius of the artwork's outer corners (white outside), fraction of its side
FRAME = 0.042   # the navy frame and its black line, fraction of the side
FOREGROUND = 84  # dp of the 108 dp adaptive canvas taken by the picture


def artwork():
    art = Image.open(SOURCE).convert("RGBA")
    s = art.size[0]
    mask = Image.new("L", art.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=s * CORNER, fill=255)
    art.putalpha(mask)  # the white corners go
    return art


def picture(art):
    s = art.size[0]
    inset = round(s * FRAME)
    return art.crop((inset, inset, s - inset, s - inset)).convert("RGBA")


def foreground(art, px):
    canvas = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    side = round(px * FOREGROUND / 108)
    canvas.alpha_composite(picture(art).resize((side, side), Image.LANCZOS), ((px - side) // 2, (px - side) // 2))
    return canvas


def round_icon(art, px):
    crop = picture(art)
    mask = Image.new("L", crop.size, 0)
    ImageDraw.Draw(mask).ellipse([0, 0, crop.size[0] - 1, crop.size[1] - 1], fill=255)
    crop.putalpha(mask)
    return crop.resize((px, px), Image.LANCZOS)


def main(res):
    art = artwork()
    for name, k in DENSITIES:
        os.makedirs(f"{res}/mipmap-{name}", exist_ok=True)
        os.makedirs(f"{res}/drawable-{name}", exist_ok=True)
        art.resize((round(48 * k), round(48 * k)), Image.LANCZOS).save(f"{res}/mipmap-{name}/ic_launcher.png")
        round_icon(art, round(48 * k)).save(f"{res}/mipmap-{name}/ic_launcher_round.png")
        foreground(art, round(108 * k)).save(f"{res}/drawable-{name}/ic_launcher_foreground.png")


if __name__ == "__main__":
    main(sys.argv[1])
