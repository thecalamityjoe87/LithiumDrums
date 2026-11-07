"""Poses the finger-snap hand and writes hand_snap_hand.obj, hand_snap_full.obj
and hand_snap.json, which blender-kit.py loads.

    blender -b --python assets/models/pose-snap.py -- out=assets/models

Needs the MPFB extension in Blender (it builds the MakeHuman body and rig).

The fingers that don't touch anything are set by hand. The thumb and middle
finger are solved so their pads meet, because that's too fiddly to set by eye.
What the solver has to satisfy:

- The pads, not just the skin: each pad is a point and a direction on the real
  skin, found by casting a ray at it, and the two must face each other.
- The thumb's last two joints may bend backward. A thumb pressed by a finger
  does that, and it's what turns the pad up to meet the middle finger. With
  forward-only limits the pads can't face each other.
- The thumb stays in the range where it opposes the fingers. Without those
  limits the solver finds answers no hand makes, like the thumb behind the palm.
- The ring and little finger stop short of the palm: much tighter than the
  curls below and their tips go through it.
"""

import json
import math
import random
import sys

import bmesh
import bpy
from mathutils import Euler, Vector
from mathutils.bvhtree import BVHTree

args = dict(a.split('=', 1) for a in sys.argv[sys.argv.index('--') + 1:])
OUT = args.get('out', 'assets/models')

# Curls in degrees, knuckle to tip. +X on a finger bone bends it toward the palm.
INDEX, INDEX_SPLAY = (8, 12, 6), 20    # out and nearly straight, swung in toward the middle finger
INDEX_LENGTH = 0.85                    # held straight beside curled fingers it looks too long at full length
RING = (54, 72, 30)
LITTLE = (56, 72, 30)

# Where on the last bone each pad is, as a fraction of the bone's length.
THUMB_PAD_AT, MIDDLE_PAD_AT = 0.5, 0.55
PRESS = 0.0012                         # metres between the pad points: the skins just meet
THUMB_UP = 0.5                         # the thumb's tip points at least this far out of the palm
PAD_FORWARD = 0.35                     # and its pad at least this far toward the fingers

# What the solver may move: bone, axis (0 x, 1 y, 2 z), lowest, highest, start, spread (degrees).
SOLVED = [('finger1-1.R', 0, 40, 88, 66, 30), ('finger1-1.R', 1, -55, 25, -17, 30), ('finger1-1.R', 2, -88, -35, -68, 30),
          ('finger1-2.R', 0, -15, 35, 4, 15), ('finger1-2.R', 2, -10, 10, -4, 8),
          ('finger1-3.R', 0, -28, 15, -14, 12),
          ('finger3-1.R', 0, 35, 80, 70, 14), ('finger3-1.R', 2, -6, 6, 0, 6),
          ('finger3-2.R', 0, 22, 72, 38, 14), ('finger3-3.R', 0, 10, 35, 18, 10)]
RESTARTS = 20

# ---------- the body and its rig ----------
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.preferences.addon_enable(module='bl_ext.user_default.mpfb')
from bl_ext.user_default.mpfb.services.humanservice import HumanService
from bl_ext.user_default.mpfb.services.targetservice import TargetService

macro = TargetService.get_default_macro_info_dict()
macro['gender'] = 0.85
base = HumanService.create_human(mask_helpers=True, detailed_helpers=True, extra_vertex_groups=False, feet_on_ground=True, scale=0.1, macro_detail_dict=macro)
rig = HumanService.add_builtin_rig(base, 'default')
bones = rig.pose.bones
for b in bones:
    b.rotation_mode = 'XYZ'


def cut_out(prefixes, name):
    """The posed skin of the bones whose names start with `prefixes`, on the right side."""
    bpy.context.view_layer.update()
    graph = bpy.context.evaluated_depsgraph_get()
    posed = bpy.data.meshes.new_from_object(base.evaluated_get(graph), preserve_all_data_layers=True, depsgraph=graph)
    groups = {g.index: g.name for g in base.vertex_groups}
    keep = [sum(g.weight for g in v.groups if groups.get(g.group, '').startswith(prefixes) and groups[g.group].endswith('.R')) > 0.5 for v in posed.vertices]
    bm = bmesh.new()
    bm.from_mesh(posed)
    bm.verts.ensure_lookup_table()
    bmesh.ops.delete(bm, geom=[bm.verts[i] for i, k in enumerate(keep) if not k], context='VERTS')
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    o = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(o)
    for p in mesh.polygons:
        p.use_smooth = True
    return o


# ---------- forward kinematics, so solving doesn't re-evaluate the whole body ----------
relative = {b.name: (b.parent.bone.matrix_local.inverted() @ b.bone.matrix_local if b.parent else b.bone.matrix_local.copy()) for b in bones}
parent = {b.name: (b.parent.name if b.parent else None) for b in bones}
pose = {}                                  # bone name -> (x, y, z) degrees


def bone_matrix(name):
    m = relative[name] @ Euler([math.radians(v) for v in pose.get(name, (0, 0, 0))], 'XYZ').to_matrix().to_4x4()
    return bone_matrix(parent[name]) @ m if parent[name] else m


# ---------- the pads, on the real skin at rest ----------
rest = cut_out(('finger', 'metacarpal', 'wrist'), 'rest')
skin = BVHTree.FromPolygons([v.co.copy() for v in rest.data.vertices], [tuple(p.vertices) for p in rest.data.polygons])


def pad(bone, along):
    """The skin point and its outward normal on a bone's pad, in the bone's own frame. The pad is on its +Z side (the nail is on -Z)."""
    b = bones[bone].bone
    m = b.matrix_local
    on_bone = m @ Vector((0, b.length * along, 0))
    z = (m.to_3x3() @ Vector((0, 0, 1))).normalized()
    hit, normal, _, _ = skin.ray_cast(on_bone + z * 0.05, -z)
    assert hit is not None, bone
    return m.inverted() @ hit, (m.inverted().to_3x3() @ normal).normalized()


thumb_point, thumb_normal = pad('finger1-3.R', THUMB_PAD_AT)
middle_point, middle_normal = pad('finger3-3.R', MIDDLE_PAD_AT)
palm_frame = bones['metacarpal3.R'].bone.matrix_local.to_3x3()
FINGERS, PALM = palm_frame @ Vector((0, 1, 0)), palm_frame @ Vector((0, 0, 1))

# ---------- the fingers set by hand ----------
pose.update({'finger2-1.R': (INDEX[0], 0, INDEX_SPLAY), 'finger2-2.R': (INDEX[1], 0, 0), 'finger2-3.R': (INDEX[2], 0, 0)})
for n, curl in ((4, RING), (5, LITTLE)):
    for i in range(3):
        pose['finger%d-%d.R' % (n, i + 1)] = (curl[i], 0, 0)


# ---------- the thumb and middle finger ----------
def apply(values):
    angles = {}
    for (bone, axis, *_), value in zip(SOLVED, values):
        angles.setdefault(bone, [0, 0, 0])[axis] = value
    pose.update({bone: tuple(a) for bone, a in angles.items()})


def measure():
    """The gap, how flat the pads face (-1 is flush), how far the thumb points out of the palm, how far its pad faces the fingers."""
    t, m = bone_matrix('finger1-3.R'), bone_matrix('finger3-3.R')
    t_normal = (t.to_3x3() @ thumb_normal).normalized()
    m_normal = (m.to_3x3() @ middle_normal).normalized()
    tip = (t.to_3x3() @ Vector((0, 1, 0))).normalized()
    return ((t @ thumb_point) - (m @ middle_point)).length, t_normal.dot(m_normal), tip.dot(PALM), t_normal.dot(FINGERS)


def cost(values):
    apply(values)
    gap, facing, up, forward = measure()
    total = ((gap - PRESS) / 0.0005) ** 2 + ((1 + facing) / 0.04) ** 2 + (max(0, THUMB_UP - up) / 0.04) ** 2 + (max(0, PAD_FORWARD - forward) / 0.02) ** 2
    for (_, _, _, _, start, spread), value in zip(SOLVED, values):
        total += ((value - start) / spread) ** 2 * 0.08     # stay near the natural start
    return total


random.seed(7)
best = None
for run in range(RESTARTS):
    x = [min(hi, max(lo, start + random.uniform(-1, 1) * spread * (0 if run == 0 else 1))) for _, _, lo, hi, start, spread in SOLVED]
    c, step = cost(x), 16.0
    while step > 0.1:                      # coordinate descent, halving the step when nothing helps
        improved = False
        for i, (_, _, lo, hi, _, _) in enumerate(SOLVED):
            for sign in (1, -1):
                old = x[i]
                x[i] = min(hi, max(lo, old + sign * step))
                new = cost(x)
                if new < c - 1e-9:
                    c, improved = new, True
                else:
                    x[i] = old
        if not improved:
            step /= 2
    if best is None or c < best[0]:
        best = (c, list(x))

apply(best[1])
gap, facing, up, forward = measure()
print('SOLVED gap %.2f mm, pads facing %.3f, thumb up %.2f, pad forward %.2f' % (gap * 1000, facing, up, forward))

# ---------- write it out ----------
for name, angles in pose.items():
    bones[name].rotation_euler = [math.radians(v) for v in angles]

# The index finger's length: each bone scaled along itself and not inheriting
# its parent's scale, so the finger gets shorter but no thinner.
for i in (1, 2, 3):
    bones['finger2-%d.R' % i].scale = (1, INDEX_LENGTH, 1)
    bones['finger2-%d.R' % i].bone.inherit_scale = 'NONE'

hand = cut_out(('finger', 'metacarpal', 'wrist'), 'hand')
full = cut_out(('finger', 'metacarpal', 'wrist', 'lowerarm'), 'full')
for o, part in ((hand, 'hand'), (full, 'full')):
    bpy.ops.object.select_all(action='DESELECT')
    o.select_set(True)
    bpy.context.view_layer.objects.active = o
    bpy.ops.wm.obj_export(filepath='%s/hand_snap_%s.obj' % (OUT, part), export_selected_objects=True, export_materials=False, export_normals=True, apply_modifiers=False)

# How the hand lay at rest, which load_hand() in blender-kit.py turns into place from.
json.dump({'wrist': list(bones['wrist.R'].bone.head_local), 'finger_dir': list(FINGERS), 'palm_dir': list(PALM)}, open('%s/hand_snap.json' % OUT, 'w'))
