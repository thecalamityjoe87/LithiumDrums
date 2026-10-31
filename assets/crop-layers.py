#!/usr/bin/env python3
"""Crops the full-frame layer renders from blender-kit.py into kit-renders/.

    blender -b -P assets/blender-kit.py -- mode=layers scale=0.75 samples=128
    python3 assets/crop-layers.py

Each piece is cropped to the bounding box of its visible pixels, and its rect
is written to kit-renders/layout.json. base.png (stands, hardware and floor
shadow) stays full-frame. Needs Pillow (pip install pillow).
"""

import json
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'layers')
DST = os.path.join(HERE, 'kit-renders')
STAGE_1X = (1280, 520)
DRAW_ORDER = ['base', 'china', 'ride', 'crash', 'hihat', 'floor', 'kick', 'tom2', 'tom1', 'snare']

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

with open(os.path.join(DST, 'layout.json'), 'w') as f:
    json.dump(layout, f, indent=2)
    f.write('\n')

print('Wrote', len(pieces), 'layers at render scale', render_scale)
