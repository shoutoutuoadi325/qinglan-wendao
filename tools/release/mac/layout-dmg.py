"""Create the install artwork and Finder layout without automating Finder.

Run with Pillow, ds_store and mac_alias installed in a project-local venv.
The alias is written while the HFS+ image is mounted so it resolves on download.
"""

import argparse
import shutil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from ds_store import DSStore
from mac_alias import Alias


def artwork(root: Path, preview: Path):
    width, height = 760, 540
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / height
        draw.line((0, y, width, y), fill=(int(13 + 6*t), int(35 + 12*t), int(34 + 8*t)))
    font_dir = Path("/System/Library/Fonts/Supplemental")
    title = ImageFont.truetype(str(font_dir / "Arial.ttf"), 38)
    body = ImageFont.truetype(str(font_dir / "Arial.ttf"), 17)
    small = ImageFont.truetype(str(font_dir / "Arial.ttf"), 13)
    gold, pale = (204, 178, 111), (223, 231, 218)
    draw.text((48, 35), "Q I N G L A N", font=title, fill=pale)
    draw.text((50, 87), "PATH OF ASCENSION", font=small, fill=gold)
    draw.line((48, 119, 712, 119), fill=gold, width=1)
    draw.text((48, 144), "Drag Qinglan to Applications to install", font=body, fill=pale)
    draw.line((328, 260, 430, 260), fill=gold, width=3)
    draw.line((418, 249, 430, 260, 418, 271), fill=gold, width=3)
    draw.line((48, 358, 712, 358), fill=(55, 79, 66), width=1)
    draw.text((48, 376), "Installation guide, credits and license", font=small, fill=pale)
    draw.text((48, 510), "macOS 14+  /  Apple Silicon", font=small, fill=gold)
    folder = root / ".background"
    folder.mkdir(exist_ok=True)
    image.save(folder / "install.png")
    shutil.copy2(folder / "install.png", preview)


def layout(root: Path, name: str):
    app = root / (name + ".app")
    if not app.is_dir() or not (root / "Applications").is_symlink():
        raise SystemExit("DMG payload is incomplete")
    alias = Alias.for_file(str(root / ".background/install.png"))
    with DSStore.open(str(root / ".DS_Store"), "w+") as store:
        store["."]["bwsp"] = {
            "WindowBounds": "{{160, 120}, {760, 540}}",
            "ShowToolbar": False,
            "ShowStatusBar": False,
            "ShowPathbar": False,
            "ShowSidebar": False,
            "ContainerShowSidebar": False,
            "SidebarWidth": 0,
        }
        store["."]["icvp"] = {
            "viewOptionsVersion": 1,
            "backgroundType": 2,
            "backgroundImageAlias": alias.to_bytes(),
            "iconSize": 96.0,
            "textSize": 13.0,
            "gridSpacing": 100.0,
            "gridOffsetX": 0.0,
            "gridOffsetY": 0.0,
            "arrangeBy": "none",
            "labelOnBottom": True,
            "showIconPreview": False,
            "showItemInfo": False,
        }
        store["."]["vSrn"] = ("long", 1)
        store["."]["vstl"] = ("type", b"icnv")
        for filename, point in [(app.name, (200, 260)), ("Applications", (560, 260)),
                                ("INSTALL.txt", (132, 443)), ("CREDITS.txt", (380, 443)),
                                ("EULA.txt", (628, 443))]:
            store[filename]["Iloc"] = point


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("name")
    parser.add_argument("preview", type=Path, nargs="?")
    parser.add_argument("--mounted", action="store_true")
    args = parser.parse_args()
    if args.mounted:
        layout(args.root, args.name)
    else:
        if args.preview is None:
            parser.error("preview path is required when creating artwork")
        artwork(args.root, args.preview)
