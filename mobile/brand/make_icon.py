#!/usr/bin/env python3
"""Draws the MyFiles logo and writes every icon size the platforms need.

    python3 mobile/brand/make_icon.py

A manila file folder on the app's teal, with a scanned page peeking out (its QR corner hints at the
file labels) and a "MY FILES" label on the front. Drawn at 4x and downsampled for clean edges.
Requires Pillow.
"""
import os
import subprocess
import shutil
import tempfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
MOBILE = os.path.dirname(HERE)
FONT_BOLD = os.path.join(MOBILE, "assets", "fonts", "NotoSans-Bold.ttf")

TEAL_TOP, TEAL_BOTTOM = (0x27, 0x76, 0x70), (0x14, 0x45, 0x42)
TEAL_TEXT = (0x1F, 0x5F, 0x5B)
FOLDER_BACK = (0xC4, 0x83, 0x2A)
FOLDER_FRONT_TOP, FOLDER_FRONT_BOTTOM = (0xF6, 0xBE, 0x5E), (0xE3, 0x9A, 0x31)
PAPER, PAPER_LINE = (0xFF, 0xFD, 0xF7), (0xD9, 0xD2, 0xC2)
SHADOW = (0, 0, 0, 70)


def vertical_gradient(size, top, bottom):
    w, h = size
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(h - 1, 1)
        grad.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return grad.resize((w, h))


def finder(d, x, y, s, color):
    """One QR finder pattern (7x7 modules, module size s)."""
    d.rectangle([x, y, x + 7 * s - 1, y + 7 * s - 1], fill=color)
    d.rectangle([x + s, y + s, x + 6 * s - 1, y + 6 * s - 1], fill=PAPER)
    d.rectangle([x + 2 * s, y + 2 * s, x + 5 * s - 1, y + 5 * s - 1], fill=color)


def draw(size=1024, background=True, scale=4):
    S = size * scale
    k = S / 1024  # design is laid out on a 1024 grid

    def r(*v):
        return [round(c * k) for c in v]

    img = vertical_gradient((S, S), TEAL_TOP, TEAL_BOTTOM).convert("RGBA") if background else Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Soft shadow under the folder
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(r(186, 470, 846, 842), radius=round(56 * k), fill=SHADOW)
    shadow = shadow.resize((S // 8, S // 8), Image.LANCZOS).resize((S, S), Image.LANCZOS)
    img.alpha_composite(shadow)

    d = ImageDraw.Draw(img)
    # Back of the folder with its tab
    d.rounded_rectangle(r(196, 236, 470, 340), radius=round(36 * k), fill=FOLDER_BACK)
    d.rectangle(r(196, 290, 470, 340), fill=FOLDER_BACK)  # join tab and panel without a notch
    d.rounded_rectangle(r(196, 290, 828, 800), radius=round(48 * k), fill=FOLDER_BACK)

    # Scanned page peeking out
    d.rounded_rectangle(r(300, 214, 776, 640), radius=round(26 * k), fill=PAPER)
    for i, (x0, x1) in enumerate([(344, 580), (344, 620), (344, 600), (344, 540)]):
        y = 300 + i * 52
        d.rounded_rectangle(r(x0, y, x1, y + 22), radius=round(11 * k), fill=PAPER_LINE)
    m = round(9 * k)  # QR corner: two finder patterns and a few modules
    qx, qy = round(642 * k), round(250 * k)
    finder(d, qx, qy, m, TEAL_TEXT)
    for cx, cy in [(8, 0), (9, 2), (8, 4), (10, 5), (9, 7), (11, 8), (8, 9), (10, 10)]:
        d.rectangle([qx + cx * m, qy + cy * m, qx + (cx + 1) * m - 1, qy + (cy + 1) * m - 1], fill=TEAL_TEXT)

    # Front of the folder (gradient, rounded)
    front_box = r(172, 430, 852, 820)
    fw, fh = front_box[2] - front_box[0], front_box[3] - front_box[1]
    front = vertical_gradient((fw, fh), FOLDER_FRONT_TOP, FOLDER_FRONT_BOTTOM).convert("RGBA")
    mask = Image.new("L", (fw, fh), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, fw - 1, fh - 1], radius=round(52 * k), fill=255)
    img.paste(front, front_box[:2], mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(r(172, 430, 852, 446), radius=round(8 * k), fill=(255, 220, 150))  # top edge highlight

    # Label with the name
    lx0, ly0, lx1, ly1 = r(268, 556, 756, 718)
    d.rounded_rectangle([lx0, ly0, lx1, ly1], radius=round(30 * k), fill=PAPER)
    text = "MY FILES"
    target_w = (lx1 - lx0) * 0.80
    pt = 40
    font = ImageFont.truetype(FONT_BOLD, pt)
    while True:
        bigger = ImageFont.truetype(FONT_BOLD, pt + 4)
        if d.textlength(text, font=bigger) > target_w or pt > S:
            break
        pt += 4
        font = bigger
    bbox = d.textbbox((0, 0), text, font=font, anchor="ls")
    tw, th = bbox[2] - bbox[0], -bbox[1]
    d.text(((lx0 + lx1 - tw) / 2 - bbox[0], (ly0 + ly1 + th) / 2), text, font=font, fill=TEAL_TEXT, anchor="ls")

    return img.resize((size, size), Image.LANCZOS)


def save(img, path, rgb=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    (img.convert("RGB") if rgb else img).save(path)
    print("wrote", os.path.relpath(path, MOBILE))


def main():
    master = draw(1024)

    # iOS: single 1024 icon in an asset catalog (no alpha allowed)
    appicon = os.path.join(MOBILE, "ios", "Assets.xcassets", "AppIcon.appiconset")
    save(master, os.path.join(appicon, "icon-1024.png"), rgb=True)
    with open(os.path.join(appicon, "Contents.json"), "w") as f:
        f.write('{\n  "images" : [\n    { "filename" : "icon-1024.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" }\n'
                '  ],\n  "info" : { "author" : "xcode", "version" : 1 }\n}\n')
    with open(os.path.join(MOBILE, "ios", "Assets.xcassets", "Contents.json"), "w") as f:
        f.write('{\n  "info" : { "author" : "xcode", "version" : 1 }\n}\n')

    # Android launcher icons (square + round)
    res = os.path.join(MOBILE, "android", "app", "src", "main", "res")
    for density, px in {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}.items():
        icon = draw(px)
        rounded = Image.new("L", (px * 4, px * 4), 0)
        ImageDraw.Draw(rounded).rounded_rectangle([0, 0, px * 4 - 1, px * 4 - 1], radius=px * 4 // 5, fill=255)
        sq = icon.copy()
        sq.putalpha(rounded.resize((px, px), Image.LANCZOS))
        save(sq, os.path.join(res, f"mipmap-{density}", "ic_launcher.png"))
        circle = Image.new("L", (px * 4, px * 4), 0)
        ImageDraw.Draw(circle).ellipse([0, 0, px * 4 - 1, px * 4 - 1], fill=255)
        rd = icon.copy()
        rd.putalpha(circle.resize((px, px), Image.LANCZOS))
        save(rd, os.path.join(res, f"mipmap-{density}", "ic_launcher_round.png"))

    # In-app header mark and the macOS app icon
    logo = draw(192)
    corners = Image.new("L", (768, 768), 0)
    ImageDraw.Draw(corners).rounded_rectangle([0, 0, 767, 767], radius=170, fill=255)  # iOS-like squircle-ish
    logo.putalpha(corners.resize((192, 192), Image.LANCZOS))
    save(logo, os.path.join(MOBILE, "assets", "images", "logo.png"))
    save(master, os.path.join(HERE, "icon-1024.png"))
    if shutil.which("iconutil"):
        tmp = tempfile.mkdtemp()
        iconset = os.path.join(tmp, "MyFiles.iconset")
        os.makedirs(iconset)
        for px in (16, 32, 128, 256, 512):
            draw(px).save(os.path.join(iconset, f"icon_{px}x{px}.png"))
            draw(px * 2).save(os.path.join(iconset, f"icon_{px}x{px}@2x.png"))
        out = os.path.join(HERE, "MyFiles.icns")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
        shutil.rmtree(tmp)
        print("wrote", os.path.relpath(out, MOBILE))


if __name__ == "__main__":
    main()
