"""Renders installer/wbshterm.ico: a terminal window on the same dark tile
as the shell's icon, so the two read as a pair in a Start menu.

Everything is drawn at 1024px and downsampled per size, and the sizes
under 32px drop the window chrome for a prompt and cursor alone, which
is all that survives at taskbar scale.

    python tools/make_icon.py [out.ico]
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

TILE       = (0x0E, 0x10, 0x16, 255)
TILE_EDGE  = (0x2A, 0x2D, 0x3A, 255)
WINDOW     = (0x1E, 0x1E, 0x2E, 255)
TITLE_BAR  = (0x31, 0x32, 0x44, 255)
DOT_RED    = (0xF3, 0x8B, 0xA8, 255)
DOT_YELLOW = (0xF9, 0xE2, 0xAF, 255)
DOT_GREEN  = (0xA6, 0xE3, 0xA1, 255)
PROMPT     = (0x50, 0xE3, 0x82, 255)
CURSOR     = (0x89, 0xB4, 0xFA, 255)

SIZES = [16, 24, 32, 48, 64, 128, 256]
CANVAS = 1024
FONT_PATH = Path("C:/Windows/Fonts/consolab.ttf")


def scale(value):
    return int(value * CANVAS / 256)


def draw_tile(draw):
    radius = scale(56)
    draw.rounded_rectangle((0, 0, CANVAS - 1, CANVAS - 1), radius=radius, fill=TILE_EDGE)
    inset = scale(3)
    draw.rounded_rectangle((inset, inset, CANVAS - 1 - inset, CANVAS - 1 - inset),
                           radius=radius - inset, fill=TILE)


def draw_window(draw):
    left, top, right, bottom = scale(22), scale(34), scale(234), scale(226)
    radius = scale(18)
    draw.rounded_rectangle((left, top, right, bottom), radius=radius, fill=WINDOW)

    bar_bottom = top + scale(32)
    draw.rounded_rectangle((left, top, right, bar_bottom + radius), radius=radius, fill=TITLE_BAR)
    draw.rectangle((left, bar_bottom, right, bar_bottom + radius), fill=WINDOW)

    dot = scale(6)
    cy = top + scale(15)
    for index, colour in enumerate((DOT_RED, DOT_YELLOW, DOT_GREEN)):
        cx = left + scale(18) + index * scale(18)
        draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=colour)


def draw_prompt(draw, glyph_top, glyph_height, cursor_gap):
    font = ImageFont.truetype(str(FONT_PATH), glyph_height)
    box = draw.textbbox((0, 0), "$", font=font)
    glyph_width = box[2] - box[0]

    cursor_width = int(glyph_width * 0.7)
    total = glyph_width + cursor_gap + cursor_width
    x = (CANVAS - total) // 2
    draw.text((x - box[0], glyph_top - box[1]), "$", font=font, fill=PROMPT)

    cursor_left = x + glyph_width + cursor_gap
    cursor_top = glyph_top + int((box[3] - box[1]) * 0.1)
    cursor_bottom = glyph_top + (box[3] - box[1])
    draw.rounded_rectangle((cursor_left, cursor_top, cursor_left + cursor_width, cursor_bottom),
                           radius=scale(4), fill=CURSOR)


def render(with_chrome):
    image = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw_tile(draw)

    if with_chrome:
        draw_window(draw)
        draw_prompt(draw, glyph_top=scale(96), glyph_height=scale(112), cursor_gap=scale(14))
    else:
        draw_prompt(draw, glyph_top=scale(52), glyph_height=scale(150), cursor_gap=scale(16))

    return image


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "installer" / "wbshterm.ico"
    detailed = render(with_chrome=True)
    simple = render(with_chrome=False)

    frames = []
    for size in SIZES:
        source = detailed if size >= 32 else simple
        frames.append(source.resize((size, size), Image.LANCZOS))

    frames[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    print(f"wrote {out} with sizes {SIZES}")


if __name__ == "__main__":
    main()
