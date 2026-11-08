# Renders the mics the Mixer's Mics card draws (src/ui/MicsModule.cpp): each
# on its stand, with its cable, from the front and a little above (like the
# kit's camera), on a transparent background with a soft floor shadow. The
# stands are upright, so each render is in a fixed pose: the close mic angled
# down at the drum from a clip on its rim (a cymbal's on a tall boom stand),
# the kick mic level on a low stand,
# the overhead pair hanging straight down on their cables, the room pair on
# stands facing left. mics.json gives
# each render's size, its mics' fronts (where the card starts their pickup
# lines), the stand's foot (where it meets the floor) and, for the close mic,
# its clip (which sits on the drum's rim), as shares of the image's width and
# height; and pxPerMetre, the render's scale.
#
#   blender -b -P assets/blender-mics.py -- [out=<folder>] [samples=128] [px=900] [only=<render>]
#
# Without out=, it writes assets/mic-renders/ (embedded by CMakeLists.txt).
#
# The mics are generic shapes, not copies of any make or model:
#   close        a drum mic: short gunmetal body, dark mesh grille (snare, toms, hats, cymbals)
#   kick         the same kind, bigger, with a tall mesh dome
#   overhead     a slim dark grey pencil condenser (the overhead pair)
#   room         a square large-diaphragm condenser in the same style (the room pair), side-address:
#                its front faces +X, so the body stands upright
#
# Same look as the kit (assets/blender-kit.py): Cycles, AgX, the studio world.

import bpy, math, sys, os, json
from mathutils import Vector

args = dict(a.split('=') for a in sys.argv[sys.argv.index('--') + 1:] if '=' in a) if '--' in sys.argv else {}
OUT = args.get('out', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mic-renders'))
SAMPLES = int(args.get('samples', '128'))
PX = int(args.get('px', '900'))          # the longer side of each render
ONLY = args.get('only')                  # one mic's name, to redo just that one

os.makedirs(OUT, exist_ok=True)

# ---------- scene ----------
bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene
scn.render.engine = 'CYCLES'
scn.cycles.device = 'CPU'
scn.cycles.samples = SAMPLES
scn.cycles.use_denoising = True
scn.cycles.sample_clamp_indirect = 3.0
scn.render.film_transparent = True
scn.render.image_settings.file_format = 'PNG'
scn.render.image_settings.color_mode = 'RGBA'
scn.view_settings.view_transform = 'AgX'
scn.view_settings.look = 'AgX - Medium High Contrast'
scn.view_settings.exposure = float(args.get('exposure', '-1.2'))

world = bpy.data.worlds.new('W'); scn.world = world
world.use_nodes = True
wn = world.node_tree.nodes; wl = world.node_tree.links
env = wn.new('ShaderNodeTexEnvironment')
studio = [os.path.join(bpy.utils.resource_path(k), 'datafiles', 'studiolights', 'world', 'studio.exr') for k in ('LOCAL', 'SYSTEM')]
env.image = bpy.data.images.load(next(p for p in studio if os.path.exists(p)))
bg = wn['Background']; bg.inputs['Strength'].default_value = 0.35
wl.new(env.outputs['Color'], bg.inputs['Color'])

# ---------- materials ----------
def mat(name, color, metal=0.0, rough=0.5, coat=0.0, spec=0.5, mesh=None, slots=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; p = nt.nodes['Principled BSDF']
    p.inputs['Base Color'].default_value = (*color, 1)
    p.inputs['Metallic'].default_value = metal
    p.inputs['Roughness'].default_value = rough
    p.inputs['Specular IOR Level'].default_value = spec
    if coat:
        p.inputs['Coat Weight'].default_value = coat
        p.inputs['Coat Roughness'].default_value = 0.08
    tc = nt.nodes.new('ShaderNodeTexCoord')
    if mesh:
        # A woven wire grille: a fine checker of wire and the dark foam behind
        # it, with a matching bump, in object space so it wraps the shape evenly.
        scale, holes = mesh
        ck = nt.nodes.new('ShaderNodeTexChecker'); ck.inputs['Scale'].default_value = scale
        nt.links.new(tc.outputs['Object'], ck.inputs['Vector'])
        # Mostly wire, with the dark foam showing through a little: the
        # checker only darkens the wire by a third, so the grille reads bright.
        mix = nt.nodes.new('ShaderNodeMix'); mix.data_type = 'RGBA'
        mix.inputs['A'].default_value = (*color, 1); mix.inputs['B'].default_value = (*holes, 1)
        fac = nt.nodes.new('ShaderNodeMath'); fac.operation = 'MULTIPLY'; fac.inputs[1].default_value = 0.35
        nt.links.new(ck.outputs['Fac'], fac.inputs[0]); nt.links.new(fac.outputs[0], mix.inputs['Factor'])
        nt.links.new(mix.outputs['Result'], p.inputs['Base Color'])
        bump = nt.nodes.new('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.4; bump.inputs['Distance'].default_value = 0.0003
        nt.links.new(ck.outputs['Fac'], bump.inputs['Height'])
        nt.links.new(bump.outputs['Normal'], p.inputs['Normal'])
    if slots:
        # Slots across the grille: dark bands, cut in with a bump.
        wave = nt.nodes.new('ShaderNodeTexWave'); wave.wave_type = 'BANDS'; wave.bands_direction = 'Z'
        wave.inputs['Scale'].default_value = slots; wave.inputs['Distortion'].default_value = 0.0
        nt.links.new(tc.outputs['Object'], wave.inputs['Vector'])
        ramp = nt.nodes.new('ShaderNodeValToRGB')
        ramp.color_ramp.elements[0].position = 0.45; ramp.color_ramp.elements[0].color = (0.004, 0.004, 0.005, 1)
        ramp.color_ramp.elements[1].position = 0.55; ramp.color_ramp.elements[1].color = (*color, 1)
        nt.links.new(wave.outputs['Fac'], ramp.inputs['Fac'])
        nt.links.new(ramp.outputs['Color'], p.inputs['Base Color'])
        bump = nt.nodes.new('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.8; bump.inputs['Distance'].default_value = 0.0006
        nt.links.new(ramp.outputs['Color'], bump.inputs['Height'])
        nt.links.new(bump.outputs['Normal'], p.inputs['Normal'])
    return m

M = {
    'body': mat('body', (0.022, 0.022, 0.024), metal=0.2, rough=0.55, coat=0.1),   # matte black
    'gunmetal': mat('gunmetal', (0.11, 0.11, 0.115), metal=0.85, rough=0.38),       # a drum mic's body
    'wire': mat('wire', (0.78, 0.78, 0.80), metal=1.0, rough=0.22),                 # bright steel wire
    'darkwire': mat('darkwire', (0.20, 0.20, 0.21), metal=0.9, rough=0.35),         # a drum mic's grey grille
    'blackwire': mat('blackwire', (0.05, 0.05, 0.055), metal=0.7, rough=0.4),       # a black grille's wire
    'foam': mat('foam', (0.012, 0.012, 0.014), rough=0.95),                         # the foam behind a grille
    'chrome': mat('chrome', (0.62, 0.63, 0.65), metal=1.0, rough=0.12),
    'nickel': mat('nickel', (0.70, 0.70, 0.70), metal=1.0, rough=0.30),            # an XLR plug's barrel
    'black': mat('black', (0.015, 0.015, 0.017), rough=0.5),
    'rubber': mat('rubber', (0.012, 0.012, 0.013), rough=0.85),
    'plastic': mat('plastic', (0.02, 0.02, 0.022), rough=0.35),
}


# ---------- shapes ----------
def lathe(name, profile, m, seg=96, axis='X'):
    """A surface of revolution from (radius, position) pairs, about X (or Z)."""
    verts, faces = [], []
    for k in range(seg):
        a = 2 * math.pi * k / seg
        for (r, t) in profile:
            if axis == 'X':
                verts.append((t, r * math.cos(a), r * math.sin(a)))
            else:
                verts.append((r * math.cos(a), r * math.sin(a), t))
    n = len(profile)
    for k in range(seg):
        k2 = (k + 1) % seg
        for j in range(n - 1):
            faces.append((k * n + j, k * n + j + 1, k2 * n + j + 1, k2 * n + j))
    me = bpy.data.meshes.new(name); me.from_pydata(verts, [], faces); me.update()
    for p in me.polygons:
        p.use_smooth = True
    o = bpy.data.objects.new(name, me); scn.collection.objects.link(o)
    o.data.materials.append(M[m])
    return o

def resample(profile, step):
    """The profile with points about `step` apart along it, so a grille made
    from it gets evenly spaced rings of wire."""
    out = [profile[0]]
    for (r0, t0), (r1, t1) in zip(profile, profile[1:]):
        n = max(1, round(math.hypot(r1 - r0, t1 - t0) / step))
        out += [(r0 + (r1 - r0) * k / n, t0 + (t1 - t0) * k / n) for k in range(1, n + 1)]
    return out

def grille(name, profile, wire='wire', axis='X', step=0.0032, spokes=40, gauge=0.0007):
    """A wire grille over dark foam: the surface of revolution turned into a
    cage of rings and spokes (a wireframe), with the foam a little inside it,
    so the holes read as holes even at the card's size."""
    cage = lathe(name, resample(profile, step), wire, seg=spokes, axis=axis)
    mod = cage.modifiers.new('wire', 'WIREFRAME'); mod.thickness = gauge; mod.use_even_offset = True; mod.use_replace = True
    foam = lathe(name + '-foam', [(r * 0.93, t) for (r, t) in profile], 'foam', axis=axis)
    return [cage, foam]

def torus(name, R, r, loc, rot, m):
    bpy.ops.mesh.primitive_torus_add(major_radius=R, minor_radius=r, location=loc, rotation=rot, major_segments=96, minor_segments=16)
    o = bpy.context.object; o.name = name
    bpy.ops.object.shade_smooth()
    o.data.materials.append(M[m])
    return o

def cyl(name, r, depth, loc, rot, m):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=depth, location=loc, rotation=rot, vertices=48)
    o = bpy.context.object; o.name = name
    bpy.ops.object.shade_smooth()
    o.data.materials.append(M[m])
    return o

def round_end(r0, t0, t1, steps=10):
    """A dome from radius r0 at t0 closing to a point at t1 (a quarter ellipse)."""
    return [(r0 * math.cos(0.5 * math.pi * i / steps), t0 + (t1 - t0) * math.sin(0.5 * math.pi * i / steps)) for i in range(steps + 1)]

# Each builder makes one mic, pointing along +X (its tail at x = 0), and
# returns its objects. Generic shapes, after the common kinds of drum mic.
def close_mic():
    # A drum mic: a short gunmetal body narrowing to the tail, a bright ring,
    # then a dark grey mesh grille that swells a little wider than the body
    # and rounds off at the front.
    body = lathe('close-body', [(0.0, 0.0), (0.0100, 0.0), (0.0108, 0.004), (0.0110, 0.014),
                                (0.0150, 0.050), (0.0168, 0.068), (0.0170, 0.072)], 'gunmetal')
    ring = lathe('close-ring', [(0.0170, 0.072), (0.0180, 0.073), (0.0180, 0.078), (0.0174, 0.079)], 'chrome')
    dome = [(0.0, 0.078), (0.0176, 0.078), (0.0205, 0.088), (0.0215, 0.098)]
    dome += [(0.0215 * math.cos(0.5 * math.pi * i / 12), 0.098 + 0.022 * math.sin(0.5 * math.pi * i / 12)) for i in range(1, 13)]
    return [body, ring] + grille('close-grille', dome, wire='darkwire', step=0.0026, spokes=48)

def kick_mic():
    # The kick's drum mic: the same kind, bigger, with a tall mesh dome.
    body = lathe('kick-body', [(0.0, 0.0), (0.0120, 0.0), (0.0130, 0.005), (0.0135, 0.018),
                               (0.0200, 0.055), (0.0235, 0.075), (0.0240, 0.080)], 'gunmetal')
    ring = lathe('kick-ring', [(0.0240, 0.080), (0.0252, 0.081), (0.0252, 0.086), (0.0246, 0.087)], 'chrome')
    dome = [(0.0, 0.086), (0.0248, 0.086), (0.0290, 0.100), (0.0300, 0.118)]
    dome += [(0.0300 * math.cos(0.5 * math.pi * i / 14), 0.118 + 0.034 * math.sin(0.5 * math.pi * i / 14)) for i in range(1, 15)]
    return [body, ring] + grille('kick-grille', dome, wire='darkwire', step=0.0028, spokes=56)

def overhead_mic():
    # A small-diaphragm condenser: a slim dark grey tube, a bright ring,
    # and a short dark grey mesh grille of the same width at the front.
    body = lathe('oh-body', [(0.0, 0.0), (0.0088, 0.0), (0.0098, 0.003), (0.0100, 0.010), (0.0100, 0.098)], 'gunmetal')
    ring = lathe('oh-ring', [(0.0100, 0.098), (0.0104, 0.099), (0.0104, 0.102), (0.0100, 0.103)], 'chrome')
    return [body, ring] + grille('oh-grille', [(0.0, 0.103), (0.0099, 0.103), (0.0099, 0.126)] + round_end(0.0099, 0.126, 0.130)[1:],
                                 wire='darkwire', step=0.0022, spokes=32)

def rounded_box(name, size, loc, m, bevel, grid=0):
    """A box with rounded edges; with `grid`, its faces cut into a grid
    (2^grid cells across) for a wire grille to be made from."""
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    o = bpy.context.object; o.name = name; o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bev = o.modifiers.new('round', 'BEVEL'); bev.width = bevel; bev.segments = 4; bev.limit_method = 'NONE'
    if grid:
        sub = o.modifiers.new('grid', 'SUBSURF'); sub.subdivision_type = 'SIMPLE'; sub.levels = sub.render_levels = grid
    bpy.ops.object.shade_smooth()
    o.data.materials.append(M[m])
    return o

def room_mic():
    # A large-diaphragm condenser in the drum mics' style, but square:
    # upright along Z with its front facing +X, a gunmetal box of a body, a
    # bright band, and a box-shaped dark grey mesh headbasket with rounded
    # corners over its foam.
    body = rounded_box('room-body', (0.044, 0.040, 0.100), (0.0, 0.0, -0.045), 'gunmetal', 0.006)
    band = rounded_box('room-band', (0.046, 0.042, 0.006), (0.0, 0.0, 0.0085), 'chrome', 0.002)
    cage = rounded_box('room-head', (0.048, 0.044, 0.060), (0.0, 0.0, 0.0415), 'darkwire', 0.009, grid=4)
    wire = cage.modifiers.new('wire', 'WIREFRAME'); wire.thickness = 0.0008; wire.use_even_offset = True; wire.use_replace = True
    foam = rounded_box('room-foam', (0.044, 0.040, 0.056), (0.0, 0.0, 0.0415), 'foam', 0.008)
    return [body, band, cage, foam]

# ---------- stands and cables ----------
def tube(a, b, r, m, name='tube'):
    a, b = Vector(a), Vector(b); d = b - a
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=d.length, location=(a + b) / 2, vertices=24)
    o = bpy.context.object; o.name = name
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = d.to_track_quat('Z', 'Y')
    bpy.ops.object.shade_smooth()
    o.data.materials.append(M[m])
    return o

def ball(r, loc, m):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=r, location=loc, segments=24, ring_count=12)
    o = bpy.context.object
    bpy.ops.object.shade_smooth()
    o.data.materials.append(M[m])
    return o

def stand(x, height, hub, spread, y=0.0):
    """A mic stand: three legs from a hub to the floor, and the upright tube.
    One leg points back, two forward to the sides, as most stands are set up."""
    objs = []
    top = Vector((x, y, height))
    hub_at = Vector((x, y, hub))
    for a in (90, 210, 330):
        foot = Vector((x + spread * math.cos(math.radians(a)), y + spread * math.sin(math.radians(a)), 0.012))
        objs.append(tube(hub_at, foot, 0.0065, 'black', 'leg'))
        objs.append(ball(0.012, foot, 'rubber'))
    objs.append(tube((x, y, hub - 0.03), top, 0.0105, 'black', 'upright'))
    objs.append(tube((x, y, hub - 0.03), (x, y, hub + 0.04), 0.016, 'black', 'hub'))
    objs.append(tube((x, y, height - 0.02), (x, y, height + 0.015), 0.014, 'chrome', 'clutch'))
    return objs

def boom(pivot, front, back_len, m='black'):
    """A boom arm through the pivot: out to `front`, and back past the pivot to a counterweight."""
    pivot, front = Vector(pivot), Vector(front)
    d = (front - pivot).normalized()
    back = pivot - d * back_len
    return [tube(back, front, 0.0075, m, 'boom'),
            tube(back - d * 0.005, back + d * 0.045, 0.018, 'black', 'weight'),
            ball(0.016, pivot, 'chrome')]

def cable(points, name='cable'):
    """An XLR cable through these points, as a smooth curve with a round section."""
    cu = bpy.data.curves.new(name, 'CURVE'); cu.dimensions = '3D'
    cu.bevel_depth = 0.0032; cu.bevel_resolution = 3; cu.resolution_u = 24
    sp = cu.splines.new('BEZIER'); sp.bezier_points.add(len(points) - 1)
    for bp, pt in zip(sp.bezier_points, points):
        bp.co = Vector(pt); bp.handle_left_type = bp.handle_right_type = 'AUTO'
    o = bpy.data.objects.new(name, cu); scn.collection.objects.link(o)
    o.data.materials.append(M['rubber'])
    return o

def xlr_plug(at, toward, scale=1.0):
    """The cable's XLR plug on a mic's tail at `at`, the cable leaving toward
    `toward`: a smooth satin-nickel barrel tapering from its wide end at the
    mic to a narrower back, a small latch tab near the front, then a smooth
    black rubber boot, a little slimmer, rounding off into the cable.
    PLUG_LENGTH * scale long."""
    at = Vector(at); d = (Vector(toward) - at).normalized()
    up = d.orthogonal().normalized()
    k = scale
    barrel = [(0.0, 0.0), (0.0094, 0.0), (0.0099, 0.0008), (0.0099, 0.0030)]
    barrel += [(0.0099 - 0.0021 * (t - 0.003) / 0.031, t) for t in (0.008, 0.014, 0.020, 0.026, 0.034)]   # the taper
    barrel += [(0.0076, 0.0350), (0.0, 0.0352)]
    boot = [(0.0, 0.0340), (0.0073, 0.0340), (0.0074, 0.0360), (0.0073, 0.0500)]
    boot += [(0.0034 + 0.0039 * math.cos(0.5 * math.pi * i / 6), 0.050 + 0.008 * math.sin(0.5 * math.pi * i / 6)) for i in range(1, 7)]
    boot += [(0.0034, 0.0620)]
    scaled = lambda prof: [(r_ * k, t * k) for (r_, t) in prof]
    tab = (at + d * 0.0060 * k + up * 0.0099 * k, at + d * 0.0060 * k + up * 0.0118 * k)
    return [lathe_between('xlr-barrel', at, d, scaled(barrel), 'nickel'),
            tube(tab[0], tab[1], 0.0020 * k, 'nickel', 'xlr-latch'),
            lathe_between('xlr-boot', at, d, scaled(boot), 'rubber')]

PLUG_LENGTH = 0.062

def lathe_between(name, start, d, profile, m):
    """A surface of revolution from `start` along direction `d` ((radius, distance) pairs)."""
    o = lathe(name, profile, m, seg=32, axis='Z')
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = d.to_track_quat('Z', 'Y'); o.location = start
    return o

def mount(objs, loc, rot, scale=1.0):
    """Moves a mic built at the origin (pointing +X) to `loc`, turned by `rot`
    (XYZ Euler) and scaled. The mics on stands are drawn bigger than life
    (MIC_ON_STAND): at real size they're specks beside the stands in the card."""
    e = bpy.data.objects.new('mount', None); scn.collection.objects.link(e)
    e.location = loc; e.rotation_euler = rot; e.scale = (scale, scale, scale)
    for o in objs:
        o.parent = e
    bpy.context.view_layer.update()
    return [e] + objs

def world(e, local):
    return e.matrix_world @ Vector(local)

MIC_ON_STAND = 1.6

# Where each mic's front is, in its own space (where its pickup line starts).
TIP = {'close': (0.120, 0, 0), 'kick': (0.152, 0, 0), 'overhead': (0.130, 0, 0), 'room': (0.024, 0, 0.042)}

# ---------- the scenes ----------
# Each returns (objects, tips in world space, the point on the floor the card lines up with).
def scene_close():
    # Clipped to the drum's rim, just above the head, angled down at it: a
    # short piece of hoop (only for the clip to hold; the card puts the drum's
    # own render under it and lines the hoop up with the drum's rim), the
    # clamp, a short arm, the mic, its plug, and the cable dropping away.
    hoop_r = 0.007
    hoop = tube((-0.05, 0.0, 0.0), (0.05, 0.0, 0.0), hoop_r, 'chrome', 'hoop')
    hoop.hide_render = True   # the drum render's own hoop shows instead
    clamp = [tube((0.0, -0.012, -0.022), (0.0, -0.012, 0.014), 0.009, 'plastic', 'clamp-front'),
             tube((0.0, 0.012, -0.022), (0.0, 0.012, 0.014), 0.009, 'plastic', 'clamp-back'),
             tube((0.0, -0.014, 0.016), (0.0, 0.014, 0.016), 0.010, 'plastic', 'clamp-top'),
             tube((0.0, -0.018, -0.012), (0.0, -0.030, -0.012), 0.004, 'chrome', 'screw')]
    arm = tube((0.0, 0.0, 0.018), (-0.012, 0.0, 0.050), 0.0065, 'black', 'arm')
    tilt = math.radians(8)
    tail = Vector((-0.030, 0.0, 0.060)); d = Vector((math.cos(tilt), 0.0, -math.sin(tilt)))   # the mic's axis, tail to front
    mic = mount(close_mic(), tail, (0.0, tilt, 0.0))
    plug = xlr_plug(tail, tail - d)
    out = tail - d * PLUG_LENGTH
    lead = cable([out, out + Vector((-0.010, 0.004, -0.006)), out + Vector((-0.018, 0.010, -0.040)),
                  out + Vector((-0.020, 0.016, -0.10)), out + Vector((-0.018, 0.02, -0.17))])
    objs = [hoop] + clamp + [arm, ball(0.008, (-0.014, 0.0, 0.054), 'plastic')] + mic + plug + [lead]
    # On the rim, off the floor: no floor to cut through its grille. 'clip': the
    # hoop's middle, which the card puts on the drum render's rim.
    return objs, [world(mic[0], TIP['close'])], None, {'clip': Vector((0.0, 0.0, 0.0))}

def scene_kick():
    # A low stand in front of the kick: the boom's end holds the mic on a
    # clip under its body; the plug and cable come out of its tail.
    objs = stand(0.0, 0.18, 0.07, 0.16)
    pivot, end = (0.0, 0.0, 0.19), (0.25, 0.0, 0.215)
    objs += boom(pivot, end, 0.10)
    k = MIC_ON_STAND
    tilt = math.radians(-4)
    tail = Vector((0.20, 0.0, 0.25)); d = Vector((math.cos(tilt), 0.0, -math.sin(tilt)))
    mic = mount(kick_mic(), tail, (0.0, tilt, 0.0), k)
    objs += mic
    objs.append(tube(end, (0.25, 0.0, 0.245), 0.009, 'plastic', 'clip'))
    objs += xlr_plug(tail, tail - d, k)
    out = tail - d * PLUG_LENGTH * k
    objs.append(cable([out, out + Vector((-0.016, 0.01, -0.006)), (0.07, 0.016, 0.21), (0.025, 0.018, 0.16),
                       (0.02, 0.02, 0.05), (-0.05, 0.03, 0.004), (-0.22, 0.06, 0.004), (-0.36, 0.13, 0.004)]))
    return objs, [world(mic[0], TIP['kick'])], Vector((0.0, 0.0, 0.0))

def scene_cymbal():
    # A cymbal's or the hi-hat's close mic: a drum mic on a tall boom stand,
    # like the room mics' stands, the boom reaching up and out to the right
    # over the plate's edge, the mic at its end angled down at the plate; the
    # plug and cable run back along the boom and down the stand to the floor.
    # Drawn at the room stands' scale (the mic bigger than life, like theirs).
    height = 1.10
    objs = stand(0.0, height, 0.28, 0.27)
    pivot, end = Vector((0.0, 0.0, height + 0.01)), Vector((0.42, 0.0, height + 0.20))
    objs += boom(pivot, end, 0.20)
    k = MIC_ON_STAND
    tilt = math.radians(40)
    tail = end + Vector((0.012, 0.0, 0.006)); d = Vector((math.cos(tilt), 0.0, -math.sin(tilt)))
    mic = mount(close_mic(), tail, (0.0, tilt, 0.0), k)
    objs += mic
    objs.append(tube(end, tail + d * 0.03 * k, 0.010, 'plastic', 'clip'))
    objs += xlr_plug(tail, tail - d, k)
    out = tail - d * PLUG_LENGTH * k
    objs.append(cable([out, out + Vector((-0.02, 0.008, -0.02)), (0.28, 0.014, height + 0.12), (0.10, 0.016, height + 0.04),
                       (0.02, 0.018, height - 0.08), (0.02, 0.02, 0.10), (0.10, 0.04, 0.004), (0.36, 0.12, 0.004)]))
    return objs, [world(mic[0], TIP['close'])], Vector((0.0, 0.0, 0.0))

def scene_overheads():
    # The pair hangs over the kit, each mic straight down on its own cable,
    # as overheads are often hung from the ceiling. The cables are long
    # enough to run up out of the top of the card wherever it hangs them.
    objs, tips = [], []
    for side in (-1, 1):
        x = side * 0.22
        mic = mount(overhead_mic(), (x, 0.0, 0.0), (0.0, math.radians(90), 0.0))   # pointing down, tail up
        objs += mic
        tips.append(world(mic[0], TIP['overhead']))
        objs += xlr_plug((x, 0.0, 0.0), (x, 0.0, 1.0))
        objs.append(cable([(x, 0.0, PLUG_LENGTH), (x, 0.0, 0.09), (x + side * 0.002, 0.002, 0.30), (x + side * 0.005, 0.004, 0.60), (x + side * 0.008, 0.006, 0.90)]))
    return objs, tips, None

def scene_room():
    # Two stands, each mic on a short arm off the top of its stand (the
    # stand behind it), facing left at the kit; the plug and cable come out
    # of the mic's bottom and run down beside the stand to the floor.
    objs, tips = [], []
    k = MIC_ON_STAND
    for i, x in enumerate((0.0, 0.34)):
        h = 1.22 + 0.06 * i
        y = 0.10 * i
        objs += stand(x, h, 0.28, 0.27, y=y)
        mx = x - 0.06                       # the mic, in front of (left of) its stand
        mz = h + 0.07 * k                   # its origin: the arm meets the body a third of the way up
        mic = mount(room_mic(), (mx, y, mz), (0.0, 0.0, math.pi), k)   # facing -X: to the left, at the kit
        objs += mic
        objs.append(tube((x, y, h), (x, y, h + 0.012), 0.009, 'chrome', 'thread'))
        objs.append(tube((x, y, h + 0.006), (mx + 0.020 * k, y, h + 0.006), 0.0065, 'black', 'arm'))
        tips.append(world(mic[0], TIP['room']))
        tail = Vector((mx, y, mz - 0.095 * k))
        objs += xlr_plug(tail, tail - Vector((0, 0, 1)), k)
        out = tail - Vector((0.0, 0.0, PLUG_LENGTH * k))
        objs.append(cable([out, out + Vector((0.004, 0.006, -0.05)), (x - 0.012, y + 0.016, h - 0.35),
                           (x - 0.01, y + 0.02, 0.10), (x + 0.08, y + 0.04, 0.004), (x + 0.36, y + 0.12, 0.004)]))
    return objs, tips, Vector((0.14, 0.05, 0.0))

SCENES = {
    'mics-close': scene_close,
    'mics-kick': scene_kick,
    'mics-cymbal': scene_cymbal,
    'mics-overheads': scene_overheads,
    'mics-room': scene_room,
}

# ---------- lights, floor, camera ----------
def area(loc, energy, size, color=(1, 1, 1), target=(0, 0, 0.6)):
    ld = bpy.data.lights.new('A', 'AREA'); ld.energy = energy; ld.size = size; ld.color = color
    o = bpy.data.objects.new('A', ld); o.location = loc
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = (Vector(target) - Vector(loc)).to_track_quat('-Z', 'Y')
    scn.collection.objects.link(o)

# The kit's three lights (assets/blender-kit.py), so the mics sit with it.
area((-1.6, -2.2, 3.0), 520, 2.2)
area((2.2, -1.6, 1.8), 160, 1.6, (0.85, 0.92, 1.0))
area((0.4, 2.4, 2.6), 650, 1.4, (1.0, 0.95, 0.88))

# A shadow catcher, so the stands' feet and cables cast a soft shadow on the
# floor (and only that shows, like the kit's floor shadow).
bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
floor = bpy.context.object; floor.is_shadow_catcher = True

cd = bpy.data.cameras.new('C'); cd.type = 'ORTHO'
cam = bpy.data.objects.new('C', cd); scn.collection.objects.link(cam); scn.camera = cam
TILT = math.radians(float(args.get('tilt', '12')))   # looking slightly down, as the kit's camera does
cam.rotation_mode = 'XYZ'; cam.rotation_euler = (math.pi / 2 - TILT, 0.0, 0.0)

def frame(objs):
    """Fits the orthographic camera to the objects, with a margin."""
    bpy.context.view_layer.update()
    inv = cam.matrix_world.inverted()
    pts = []
    for o in objs:
        if o.type in ('MESH', 'CURVE'):
            pts += [inv @ (o.matrix_world @ Vector(c)) for c in o.bound_box]
    x0, x1 = min(p.x for p in pts), max(p.x for p in pts)
    y0, y1 = min(p.y for p in pts), max(p.y for p in pts)
    w, h = (x1 - x0) * 1.06, (y1 - y0) * 1.06
    centre = cam.matrix_world @ Vector(((x0 + x1) / 2, (y0 + y1) / 2, 0))
    cam.location = centre - cam.matrix_world.to_3x3() @ Vector((0, 0, -3.0))
    cd.ortho_scale = max(w, h)
    if w >= h:
        scn.render.resolution_x, scn.render.resolution_y = PX, max(8, round(PX * h / w))
    else:
        scn.render.resolution_x, scn.render.resolution_y = max(8, round(PX * w / h)), PX
    bpy.context.view_layer.update()

def to_image(p):
    """A world point's place in the render, as a share of its width and height (y down)."""
    from bpy_extras.object_utils import world_to_camera_view
    v = world_to_camera_view(scn, cam, p)
    return [round(v.x, 4), round(1.0 - v.y, 4)]

meta = {}
meta_path = os.path.join(OUT, 'mics.json')
if ONLY and os.path.exists(meta_path):
    meta = json.load(open(meta_path))

for name, build in SCENES.items():
    if ONLY and name != ONLY:
        continue
    objs, tips, foot, *rest = build()
    extra = rest[0] if rest else {}
    floor.hide_render = foot is None   # the overheads hang in the air; the close mic is on the rim
    frame(objs)
    scn.render.filepath = os.path.join(OUT, name + '.png')
    bpy.ops.render.render(write_still=True)
    meta[name] = {'size': [scn.render.resolution_x, scn.render.resolution_y],
                  'pxPerMetre': round(max(scn.render.resolution_x, scn.render.resolution_y) / cd.ortho_scale, 2),
                  'tips': [to_image(t) for t in tips],
                  'foot': to_image(foot) if foot is not None else None}
    for key, point in extra.items():
        meta[name][key] = to_image(point)
    for o in objs:
        bpy.data.objects.remove(o, do_unlink=True)
    print('rendered', scn.render.filepath)

with open(meta_path, 'w') as f:
    json.dump(meta, f, indent=2)
