"""Builds res/PSCam4Win.ico.

If ps3eye_source.png exists next to this script (e.g. real PlayStation Eye
logo art), it is center-fitted onto a transparent square and used directly.
Otherwise a stylized PS3 Eye device is drawn: glossy black pill body, chrome
lens ring with the characteristic blue tint, red status LED.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "ps3eye_source.png")
OUT = os.path.join(HERE, "PSCam4Win.ico")
SIZES = [256, 64, 48, 32, 24, 20, 16]
S = 1024  # supersampled master canvas


def load_source():
    im = Image.open(SOURCE).convert("RGBA")
    side = max(im.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(im, ((side - im.width) // 2, (side - im.height) // 2), im)
    return canvas


def draw_device():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = S // 2, S // 2

    # Pill-shaped body with a subtle vertical sheen
    body = (60, 332, 964, 692)
    d.rounded_rectangle(body, radius=180, fill=(24, 25, 29, 255))
    d.rounded_rectangle((body[0] + 14, body[1] + 14, body[2] - 14, cy + 10),
                        radius=166, fill=(38, 40, 46, 255))
    d.rounded_rectangle(body, radius=180, outline=(70, 72, 80, 255), width=6)

    # Lens stack: chrome ring -> blue accent -> glass -> iris
    r = 250
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(185, 190, 200, 255))
    r = 226
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(13, 16, 22, 255))
    r = 222
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(46, 111, 216, 255), width=14)
    r = 196
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(10, 14, 24, 255))
    r = 120
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(16, 38, 84, 255))
    r = 86
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(8, 10, 16, 255))

    # Glass highlight (upper-left arc)
    hl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse((cx - 170, cy - 180, cx + 40, cy + 10), fill=(255, 255, 255, 70))
    hd.ellipse((cx - 130, cy - 150, cx + 60, cy + 40), fill=(0, 0, 0, 0))
    im = Image.alpha_composite(im, hl)
    d = ImageDraw.Draw(im)

    # Red status LED on the right of the body
    d.ellipse((858, 492, 898, 532), fill=(214, 40, 40, 255))
    d.ellipse((864, 498, 880, 514), fill=(255, 140, 140, 255))
    return im


def main():
    master = load_source() if os.path.exists(SOURCE) else draw_device()
    imgs = [master.resize((s, s), Image.LANCZOS) for s in SIZES]
    imgs[0].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES],
                 append_images=imgs[1:])
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
