#!/usr/bin/env python3
"""Crops the full-frame layer renders from blender-kit.py into kit-renders/.

    blender -b -P assets/blender-kit.py -- mode=layers scale=0.75 samples=128
    blender -b -P assets/blender-kit.py -- mode=pieces scale=2.25 samples=128
    python3 assets/crop-layers.py

Each piece is cropped to the bounding box of its visible pixels, and its rect
is written to kit-renders/layout.json. base.png (stands, hardware and floor
shadow) stays full-frame. Needs Pillow (pip install pillow).
"""

import json
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter


def fade_beyond(img, keep, solid=0.04, gone=0.26, steps=24):
    """Fades img's alpha out around the box `keep`: fully kept up to `solid`
    of the box's width beyond it, gone at `gone`. A cymbal keeps the start of
    its boom arm (the snare the top of its stand), and the rest fades away."""
    w = keep[2] - keep[0]
    mask = Image.new('L', img.size, 0)
    draw = ImageDraw.Draw(mask)
    for i in range(steps + 1):                     # nested boxes, outermost (faintest) first
        t = i / steps
        grow = w * (gone - (gone - solid) * t)
        draw.rectangle((keep[0] - grow, keep[1] - grow, keep[2] + grow, keep[3] + grow), fill=int(255 * t))
    mask = mask.filter(ImageFilter.GaussianBlur(w * (gone - solid) / steps))
    r, g, b, a = img.split()
    return Image.merge('RGBA', (r, g, b, ImageChops.multiply(a, mask)))


def crop_piece(src_folder, name):
    """A piece render, cropped to its pixels (and for a cymbal, its boom faded out)."""
    img = Image.open(os.path.join(src_folder, name + '.png')).convert('RGBA')
    bare = os.path.join(src_folder, name + '-bare.png')

    if os.path.exists(bare):
        img = fade_beyond(img, Image.open(bare).convert('RGBA').getchannel('A').getbbox())

    # Pixels the fade left almost transparent don't count toward the crop.
    box = img.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    return img.crop(box)


# python3 crop-layers.py --finish <kit id> <render folder>: one kit's finish of
# the drums (blender-kit.py kit=<id>, layers and pieces of kick, tom1, tom2,
# floor and snare), cropped to exactly the default layers' rectangles so they
# can stand in for them, and written as finish-<kit id>-*.png.
FINISH_DRUMS = ['kick', 'tom1', 'tom2', 'floor', 'snare']

if len(sys.argv) == 4 and sys.argv[1] == '--finish':
    kit, src = sys.argv[2], sys.argv[3]
    dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kit-renders')
    rects = json.load(open(os.path.join(dst, 'layout.json')))['pieces']
    for name in FINISH_DRUMS:
        r = rects[name]
        Image.open(os.path.join(src, 'layers', name + '.png')).convert('RGBA') \
             .crop((r['x'], r['y'], r['x'] + r['w'], r['y'] + r['h'])) \
             .save(os.path.join(dst, 'finish-%s-%s.png' % (kit, name)), optimize=True)
        crop_piece(os.path.join(src, 'pieces'), name).save(os.path.join(dst, 'finish-%s-slot-%s.png' % (kit, name)), optimize=True)
    print('Wrote the', kit, 'finish')
    sys.exit(0)

# python3 crop-layers.py --pieces <render folder> <output folder>: just the slot
# pictures, for checking a preview render.
if len(sys.argv) == 4 and sys.argv[1] == '--pieces':
    for f in sorted(os.listdir(sys.argv[2])):
        if f.endswith('.png') and not f.endswith('-bare.png'):
            crop_piece(sys.argv[2], f[:-4]).save(os.path.join(sys.argv[3], 'slot-' + f))
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'layers')
DST = os.path.join(HERE, 'kit-renders')
STAGE_1X = (1280, 744)
DRAW_ORDER = ['base', 'china', 'ride', 'crash2', 'crash', 'hihat', 'floor', 'kick', 'tom2', 'tom1', 'snare']

pieces = {}
render_scale = None

for name in DRAW_ORDER:
    img = Image.open(os.path.join(SRC, name + '.png')).convert('RGBA')
    render_scale = img.width / STAGE_1X[0]

    if name == 'base':
        box = (0, 0, img.width, img.height)
    else:
        box = img.getchannel('A').getbbox()

    img.crop(box).save(os.path.join(DST, name + '.png'), optimize=True)
    pieces[name] = {'x': box[0], 'y': box[1], 'w': box[2] - box[0], 'h': box[3] - box[1]}

layout = {
    'renderScale': render_scale,
    'stageSizeAt1x': list(STAGE_1X),
    'drawOrderBackToFront': DRAW_ORDER,
    'pieces': pieces,
}

# The slot cards' pictures (mode=pieces), if they were rendered: each piece
# alone, cropped to its pixels.
PIECES_SRC = os.path.join(HERE, 'pieces')

# They're named slot-<piece>.png, as the build embeds files by name and
# kick.png is already the stage layer.
if os.path.isdir(PIECES_SRC) and '--layers-only' not in sys.argv:
    for name in DRAW_ORDER[1:] + ['cowbell', 'chimes', 'splash']:   # and the pieces that aren't on the stage
        if not os.path.exists(os.path.join(PIECES_SRC, name + '.png')):
            print('No piece render for', name, '(slot-' + name + '.png left as it was)')
            continue

        crop_piece(PIECES_SRC, name).save(os.path.join(DST, 'slot-' + name + '.png'), optimize=True)

with open(os.path.join(DST, 'layout.json'), 'w') as f:
    json.dump(layout, f, indent=2)
    f.write('\n')

print('Wrote', len(pieces), 'layers at render scale', render_scale)
