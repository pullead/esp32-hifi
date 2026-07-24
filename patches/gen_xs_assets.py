# Generate the /common/xs, /digits/xs, /btn/xs SD-card asset folders for the
# 320x170 (TFT_LAYOUT_XS) port, derived from the stock "s" (320x240) assets in
# Content_on_SD_Card.zip.
#
# Rules:
#   common/s  -> common/xs : h<=20px icons copied 1:1 (header/footer are 20px in
#                XS too); 320x240 fullscreens center-cropped to 320x170;
#                unknown.png (station logo placeholder) resized to 63x63;
#                everything else scaled by 170/240.
#   digits/s  -> digits/xs : all images scaled by 170/240 (l digits 120->85 fit
#                the 96px XS clock window).
#   btn/s     -> btn/xs    : 40x40 -> 34x34 (XS hw_btn = 34).
#
# Output: sd_xs_pack/ in the project root — copy its contents onto the SD card
# root (merge with existing folders).
import io
import os
import zipfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIP = os.path.join(ROOT, "Content_on_SD_Card.zip")
OUT = os.path.join(ROOT, "sd_xs_pack")
PREFIX = "Content_on_SD_Card/"
SCALE = 170 / 240  # 0.7083

def save(im, rel, fmt):
    dest = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if fmt == "JPEG" and im.mode in ("RGBA", "P"):
        im = im.convert("RGB")
    im.save(dest, fmt, quality=90) if fmt == "JPEG" else im.save(dest, fmt)

def fmt_of(name):
    n = name.lower()
    if n.endswith((".jpg", ".jpeg")): return "JPEG"
    if n.endswith(".png"): return "PNG"
    if n.endswith(".bmp"): return "BMP"
    return None

z = zipfile.ZipFile(ZIP)
counts = {"common": 0, "digits": 0, "btn": 0}

for n in z.namelist():
    if not n.startswith(PREFIX) or n.endswith("/"):
        continue
    rel = n[len(PREFIX):]
    parts = rel.split("/")
    fmt = fmt_of(rel)
    if fmt is None:
        continue

    if parts[0] == "common" and len(parts) == 3 and parts[1] == "s":
        im = Image.open(io.BytesIO(z.read(n)))
        w, h = im.size
        out_rel = f"common/xs/{parts[2]}"
        if parts[2].lower().startswith("unknown"):
            im = im.resize((63, 63), Image.LANCZOS)
        elif (w, h) == (320, 240):
            im = im.crop((0, 35, 320, 205))  # center-crop to 320x170
        elif h <= 20:
            pass  # header/footer icons: keep 1:1
        else:
            im = im.resize((max(1, round(w * SCALE)), max(1, round(h * SCALE))), Image.LANCZOS)
        save(im, out_rel, fmt)
        counts["common"] += 1

    elif parts[0] == "digits" and len(parts) >= 3 and parts[1] == "s":
        im = Image.open(io.BytesIO(z.read(n)))
        w, h = im.size
        im = im.resize((max(1, round(w * SCALE)), max(1, round(h * SCALE))), Image.LANCZOS)
        out_rel = "digits/xs/" + "/".join(parts[2:])
        save(im, out_rel, fmt)
        counts["digits"] += 1

    elif parts[0] == "btn" and len(parts) == 3 and parts[1] == "s":
        im = Image.open(io.BytesIO(z.read(n)))
        im = im.resize((34, 34), Image.LANCZOS)
        out_rel = f"btn/xs/{parts[2]}"
        save(im, out_rel, fmt)
        counts["btn"] += 1

print("generated:", counts, "->", OUT)
