import bpy, math, sys, os, json
from mathutils import Vector, Euler

args = dict(a.split('=') for a in sys.argv[1:] if '=' in a)
OUT = args.get('out', os.path.dirname(os.path.abspath(__file__)))
SCALE = float(args.get('scale', '0.5'))
SAMPLES = int(args.get('samples', '32'))
MODE = args.get('mode', 'preview')  # preview | layers
FINISH = tuple(float(c) for c in args.get('finish', '0.22,0.012,0.02').split(','))
STAGE_W, STAGE_H = 1280, 744   # the whole Kit tab at 1x, so the kit and its floor are never cut off

# ---------- scene ----------
bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene
scn.render.engine = 'CYCLES'
scn.cycles.device = 'CPU'
scn.cycles.samples = SAMPLES
scn.cycles.use_denoising = True        # needs a build with OpenImageDenoise; distro packages may lack it
scn.cycles.sample_clamp_indirect = 3.0  # tames fireflies from the chrome
scn.render.film_transparent = True
scn.render.resolution_x = int(STAGE_W * 2 * SCALE)
scn.render.resolution_y = int(STAGE_H * 2 * SCALE)
scn.render.image_settings.file_format = 'PNG'
scn.render.image_settings.color_mode = 'RGBA'
scn.view_settings.view_transform = 'AgX'
scn.view_settings.look = 'AgX - Medium High Contrast'
# Calibrated on Blender 5.0 so the kit matches the original renders' brightness.
scn.view_settings.exposure = float(args.get('exposure', '-1.6'))

world = bpy.data.worlds.new('W'); scn.world = world
world.use_nodes = True
wn = world.node_tree.nodes; wl = world.node_tree.links
env = wn.new('ShaderNodeTexEnvironment')
# Official builds keep datafiles next to the binary (LOCAL); distro packages use SYSTEM.
studio = [os.path.join(bpy.utils.resource_path(k), 'datafiles', 'studiolights', 'world', 'studio.exr') for k in ('LOCAL', 'SYSTEM')]
env.image = bpy.data.images.load(next(p for p in studio if os.path.exists(p)))
bg = wn['Background']; bg.inputs['Strength'].default_value = 0.3
wl.new(env.outputs['Color'], bg.inputs['Color'])

# ---------- materials ----------
def mat(name, color, metal=0.0, rough=0.5, coat=0.0, aniso=0.0, rings=False, spec=0.5):
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; p = nt.nodes['Principled BSDF']
    p.inputs['Base Color'].default_value = (*color, 1)
    p.inputs['Metallic'].default_value = metal
    p.inputs['Roughness'].default_value = rough
    p.inputs['Specular IOR Level'].default_value = spec
    if coat:
        p.inputs['Coat Weight'].default_value = coat
        p.inputs['Coat Roughness'].default_value = 0.04
    if aniso:
        p.inputs['Anisotropic'].default_value = aniso
        t = nt.nodes.new('ShaderNodeTangent'); t.direction_type = 'RADIAL'; t.axis = 'Z'
        nt.links.new(t.outputs['Tangent'], p.inputs['Tangent'])
    if rings:
        tc = nt.nodes.new('ShaderNodeTexCoord')
        sep = nt.nodes.new('ShaderNodeSeparateXYZ'); nt.links.new(tc.outputs['Object'], sep.inputs[0])
        cmb = nt.nodes.new('ShaderNodeCombineXYZ')
        nt.links.new(sep.outputs['X'], cmb.inputs['X']); nt.links.new(sep.outputs['Y'], cmb.inputs['Y'])
        ln = nt.nodes.new('ShaderNodeVectorMath'); ln.operation = 'LENGTH'; nt.links.new(cmb.outputs[0], ln.inputs[0])
        mul = nt.nodes.new('ShaderNodeMath'); mul.operation = 'MULTIPLY'; mul.inputs[1].default_value = 900
        nt.links.new(ln.outputs['Value'], mul.inputs[0])
        sn = nt.nodes.new('ShaderNodeMath'); sn.operation = 'SINE'; nt.links.new(mul.outputs[0], sn.inputs[0])
        mr = nt.nodes.new('ShaderNodeMapRange')
        mr.inputs['From Min'].default_value = -1; mr.inputs['To Min'].default_value = rough * 0.75; mr.inputs['To Max'].default_value = rough * 1.35
        nt.links.new(sn.outputs[0], mr.inputs['Value']); nt.links.new(mr.outputs['Result'], p.inputs['Roughness'])
    return m

M = {
    'finish': mat('finish', FINISH, rough=0.38, coat=0.5, spec=0.1),   # lacquer: a half-strength coat keeps the shell's colour saturated
    'chrome': mat('chrome', (0.62, 0.63, 0.65), metal=1.0, rough=0.09),   # real chromium reflects ~60%; brighter reads as white plastic
    'head': mat('head', (0.86, 0.83, 0.76), rough=0.75),
    'reso': mat('reso', (0.018, 0.018, 0.02), rough=0.55, coat=0.15),
    'black': mat('black', (0.015, 0.015, 0.017), rough=0.5),
    'felt': mat('felt', (0.03, 0.03, 0.035), rough=0.95),
    'bronze': mat('bronze', (0.78, 0.52, 0.22), metal=1.0, rough=0.24, aniso=0.6, rings=True),
    'brass': mat('brass', (0.80, 0.60, 0.30), metal=1.0, rough=0.2, coat=0.3),
    'rubber': mat('rubber', (0.012, 0.012, 0.013), rough=0.85),
    'plastic': mat('plastic', (0.02, 0.02, 0.022), rough=0.35),
    'cast': mat('cast', (0.42, 0.43, 0.44), metal=1.0, rough=0.42),   # cast aluminium pedal parts
    'wood': mat('wood', (0.78, 0.56, 0.34), rough=0.6),                 # bare maple inside the shell
}

# ---------- kit finishes ----------
# kit=<id> renders a kit's own finish from assets/kit-finishes.json: its
# shells, hoops, lugs and heads, and its snare. The shapes are the same for
# every kit; only these materials change. Without kit=, the default red.
def finish_mat(name, spec):
    """A shell material: lacquer, pearl (mottled, like an oyster wrap), sparkle
    (metal flakes under a clear coat), stain (wood grain through a tinted
    lacquer) or metal (a steel or brass snare shell)."""
    style = spec.get('style', 'lacquer')
    c1 = tuple(spec['color'])
    c2 = tuple(spec.get('color2', [min(1.0, c * 2.2 + 0.02) for c in c1]))
    if style == 'metal':
        return mat(name, c1, metal=1.0, rough=spec.get('rough', 0.16), coat=0.2)
    m = mat(name, c1, rough=spec.get('rough', 0.35), coat=spec.get('coat', 0.6), spec=0.1)
    if style == 'lacquer':
        return m
    nt = m.node_tree; p = nt.nodes['Principled BSDF']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].color = (*c1, 1); ramp.color_ramp.elements[1].color = (*c2, 1)
    if style == 'pearl':
        # Big soft swirls, stretched round the shell like a wrap's pattern.
        mp = nt.nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0, 1.0, 0.35)
        nz = nt.nodes.new('ShaderNodeTexNoise'); nz.inputs['Scale'].default_value = 7.0
        nz.inputs['Detail'].default_value = 8.0; nz.inputs['Distortion'].default_value = 2.5
        nt.links.new(tc.outputs['Object'], mp.inputs['Vector']); nt.links.new(mp.outputs[0], nz.inputs['Vector'])
        ramp.color_ramp.elements[0].position = 0.35; ramp.color_ramp.elements[1].position = 0.7
        nt.links.new(nz.outputs['Fac'], ramp.inputs['Fac'])
    elif style == 'stain':
        # Grain running along the shell: bands stretched by noise.
        # (The shell's axis is the drum's Z, so bands across X run up and down it.)
        wv = nt.nodes.new('ShaderNodeTexWave'); wv.wave_type = 'BANDS'; wv.bands_direction = 'X'
        # Fine, low-contrast grain: the ramp spans the whole wave, so the
        # bands fade into each other rather than reading as stripes.
        wv.inputs['Scale'].default_value = 6.0; wv.inputs['Distortion'].default_value = 3.0
        wv.inputs['Detail'].default_value = 4.0; wv.inputs['Detail Scale'].default_value = 1.5
        nt.links.new(tc.outputs['Object'], wv.inputs['Vector'])
        ramp.color_ramp.elements[0].position = 0.0; ramp.color_ramp.elements[1].position = 1.0
        ramp.color_ramp.elements[1].color = (*[a + (b - a) * 0.6 for a, b in zip(c1, c2)], 1)
        nt.links.new(wv.outputs['Fac'], ramp.inputs['Fac'])
    elif style == 'sparkle':
        # Fine metal flakes: a dense cell texture picks some to catch the light.
        vo = nt.nodes.new('ShaderNodeTexVoronoi'); vo.inputs['Scale'].default_value = 900.0
        nt.links.new(tc.outputs['Object'], vo.inputs['Vector'])
        ramp.color_ramp.elements[0].position = 0.55; ramp.color_ramp.elements[1].position = 0.95
        nt.links.new(vo.outputs['Distance'], ramp.inputs['Fac'])
        mr = nt.nodes.new('ShaderNodeMapRange'); mr.inputs['To Min'].default_value = 0.55; mr.inputs['To Max'].default_value = 1.0
        nt.links.new(vo.outputs['Distance'], mr.inputs['Value']); nt.links.new(mr.outputs['Result'], p.inputs['Metallic'])
        p.inputs['Roughness'].default_value = 0.22
    nt.links.new(ramp.outputs['Color'], p.inputs['Base Color'])
    return m

FINISHES = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kit-finishes.json')))
KIT_ID = FINISHES.get('aliases', {}).get(args.get('kit', ''), args.get('kit', ''))
KIT_FINISH = FINISHES['kits'].get(KIT_ID, {})
HARDWARE = {'chrome': 'chrome', 'black': 'black', 'brass': 'brass'}

def finish_or(key, default):
    return finish_mat(key, KIT_FINISH[key]) if key in KIT_FINISH else M[default]

M['shell'] = finish_or('shell', 'finish')
M['snare_shell'] = finish_or('snare', 'brass')
M['kick_hoop'] = finish_or('kickHoop', 'shell') if 'kickHoop' in KIT_FINISH else M['shell']
M['hoop'] = finish_or('hoops', 'chrome') if isinstance (KIT_FINISH.get('hoops'), dict) else M[HARDWARE[KIT_FINISH.get('hoops', 'chrome')]]
M['lug'] = M[HARDWARE[KIT_FINISH.get('lugs', 'chrome')]]
if 'heads' in KIT_FINISH:
    M['head'] = mat('head_k', tuple(KIT_FINISH['heads']), rough=0.75)
if 'reso' in KIT_FINISH:
    M['reso'] = mat('reso_k', tuple(KIT_FINISH['reso']), rough=0.55, coat=0.15)

def smooth(o):
    for p in o.data.polygons: p.use_smooth = True

COLL = {}
def coll(name):
    if name not in COLL:
        c = bpy.data.collections.new(name); scn.collection.children.link(c); COLL[name] = c
    return COLL[name]

def place(o, parent, cname, m):
    for c in o.users_collection: c.objects.unlink(o)
    coll(cname).objects.link(o)
    if parent: o.parent = parent
    if m: o.data.materials.append(M[m])
    return o

def cyl(r, depth, loc, rot=(0, 0, 0), m='chrome', parent=None, cname='base', verts=48, fill='NGON'):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=depth, location=loc, rotation=rot, vertices=verts, end_fill_type=fill)
    o = bpy.context.object; smooth(o); return place(o, parent, cname, m)

def torus(R, r, loc, scale=(1, 1, 1), m='chrome', parent=None, cname='base'):
    bpy.ops.mesh.primitive_torus_add(major_radius=R, minor_radius=r, location=loc, major_segments=128, minor_segments=16)
    o = bpy.context.object; o.scale = scale; smooth(o); return place(o, parent, cname, m)

def blob(size, loc, m='chrome', parent=None, cname='base'):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=1, location=loc, segments=24, ring_count=12)
    o = bpy.context.object; o.scale = size; smooth(o); return place(o, parent, cname, m)

def box(size, loc, rot=(0, 0, 0), m='black', parent=None, cname='base'):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    o = bpy.context.object; o.scale = size
    bev = o.modifiers.new('b', 'BEVEL'); bev.width = min(size) * 0.25; bev.segments = 3
    return place(o, parent, cname, m)

def tube(a, b, r, m='chrome', cname='base'):
    a, b = Vector(a), Vector(b); d = b - a
    o = cyl(r, d.length, (a + b) / 2, m=m, cname=cname, verts=24)
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = d.to_track_quat('Z', 'Y')
    return o

def empty(name, loc, rot):
    e = bpy.data.objects.new(name, None); e.location = loc; e.rotation_euler = rot
    coll(name).objects.link(e); return e

# ---------- drum ----------
# The kick's port: about a quarter of the head across, right of centre and a
# little low, as on most ported front heads (drum-local x, y on the head).
PORT_R, PORT_AT = 0.075, (0.15, -0.05)

def drum(name, r, d, loc, rot, lugs, rows=1, shell='shell', reso=False, port=False, wood_hoop=False):
    e = empty(name, loc, rot)
    sh = cyl(r, d, (0, 0, 0), m=shell, parent=e, cname=name, verts=128, fill='NOTHING')
    sol = sh.modifiers.new('s', 'SOLIDIFY'); sol.thickness = 0.006
    for side in (1, -1):
        z = side * d / 2
        head_m = 'reso' if (reso and side == 1) else 'head'
        head = cyl(r - 0.002, 0.003, (0, 0, z), m=head_m, parent=e, cname=name, verts=128)
        if port and side == 1:
            # A real hole in the front head, so the inside of the drum shows through it.
            cutter = cyl(PORT_R, 0.03, (*PORT_AT, z), parent=e, cname=name, verts=96)
            cutter.hide_render = True
            cutter.hide_viewport = True
            cut = head.modifiers.new('port', 'BOOLEAN'); cut.operation = 'DIFFERENCE'; cut.object = cutter
        if wood_hoop:
            h = cyl(r + 0.012, 0.03, (0, 0, z - side * 0.006), m='kick_hoop', parent=e, cname=name, verts=128, fill='NOTHING')
            s2 = h.modifiers.new('s', 'SOLIDIFY'); s2.thickness = 0.014
            torus(r + 0.013, 0.003, (0, 0, z - side * 0.006), m='chrome', parent=e, cname=name)
        else:
            torus(r + 0.004, 0.0055, (0, 0, z + side * 0.004), scale=(1, 1, 1.9), m='hoop', parent=e, cname=name)
    for i in range(lugs):
        a = 2 * math.pi * (i + 0.5) / lugs
        cx, cy = math.cos(a), math.sin(a)
        if rows == 1:
            zs = [(0.0, d * 0.22)]
        else:
            zs = [(d * 0.26, d * 0.11), (-d * 0.26, d * 0.11)]
        for zc, hl in zs:
            blob((0.009, 0.009, hl), (cx * (r + 0.011), cy * (r + 0.011), zc), m='lug', parent=e, cname=name)
            side = 1 if zc >= 0 else -1
            if rows == 1:
                for s in (1, -1):
                    z0, z1 = s * hl, s * (d / 2 + 0.006)
                    o = cyl(0.0028, abs(z1 - z0), (cx * (r + 0.012), cy * (r + 0.012), (z0 + z1) / 2), parent=e, cname=name, verts=12)
            else:
                z0, z1 = zc + side * hl, side * (d / 2 + 0.006)
                cyl(0.0028, abs(z1 - z0), (cx * (r + 0.012), cy * (r + 0.012), (z0 + z1) / 2), parent=e, cname=name, verts=12)
            if wood_hoop:
                for s in (1, -1):
                    box((0.012, 0.02, 0.03), (cx * (r + 0.018), cy * (r + 0.018), s * (d / 2 - 0.004)), rot=(0, 0, a), m='chrome', parent=e, cname=name)
    if port:
        z = d / 2 + 0.003
        torus(PORT_R + 0.001, 0.004, (*PORT_AT, z), m='chrome', parent=e, cname=name)
        # What shows through the port: the bare wood inside the shell, with
        # the lugs' nuts and bolt ends on it.
        cyl(r - 0.0065, d - 0.004, (0, 0, 0), m='wood', parent=e, cname=name, verts=128, fill='NOTHING')
        # Each lug is bolted through the shell: a hex nut and the bolt's end
        # on the inside wall, top and bottom of the lug.
        hl = d * 0.22
        for i in range(lugs):
            ang = 2 * math.pi * (i + 0.5) / lugs
            cx, cy = math.cos(ang), math.sin(ang)
            # A close pair of bolts (3 cm apart) at each end of the lug.
            for zc in (0.07, 0.04, -0.10, -0.13):
                cyl(0.011, 0.008, (cx * (r - 0.010), cy * (r - 0.010), zc), rot=(0, math.pi / 2, ang), m='chrome', parent=e, cname=name, verts=6)
                cyl(0.004, 0.008, (cx * (r - 0.017), cy * (r - 0.017), zc), rot=(0, math.pi / 2, ang), m='chrome', parent=e, cname=name, verts=12)
    return e

# ---------- cymbal ----------
def lathe(name, profile, seg=160):
    verts, faces = [], []
    for k in range(seg):
        a = 2 * math.pi * k / seg
        for (rr, z) in profile:
            verts.append((rr * math.cos(a), rr * math.sin(a), z))
    n = len(profile)
    for k in range(seg):
        k2 = (k + 1) % seg
        for j in range(n - 1):
            faces.append((k * n + j, k * n + j + 1, k2 * n + j + 1, k2 * n + j))
    me = bpy.data.meshes.new(name); me.from_pydata(verts, [], faces); me.update()
    o = bpy.data.objects.new(name, me); scn.collection.objects.link(o); smooth(o)
    return o

def prof(R, china=False):
    k = R / 0.229
    if china:
        pts = [(0.03, 0.028), (0.11, 0.028), (0.125, 0.018), (0.15, 0.009), (0.3, 0.004), (0.6, -0.004), (0.82, -0.01), (0.88, -0.008), (0.94, 0.0), (1.0, 0.012)]
    else:
        pts = [(0.03, 0.033), (0.07, 0.032), (0.11, 0.028), (0.15, 0.02), (0.19, 0.012), (0.23, 0.009), (0.4, 0.004), (0.6, -0.003), (0.8, -0.01), (0.93, -0.015), (1.0, -0.018)]
    return [(f * R, z * k) for f, z in pts]

def cymbal(name, R, loc, rot, china=False, felt=True, parent=None, cname=None):
    e = parent or empty(name, loc, rot)
    o = lathe(name + '_c', prof(R, china))
    if parent is not None: o.location = loc
    sol = o.modifiers.new('s', 'SOLIDIFY'); sol.thickness = 0.0022
    place(o, e, cname or name, 'bronze')
    if felt:
        top = 0.036 * R / 0.229
        z0 = loc[2] if parent is not None else 0
        cyl(0.018, 0.01, (o.location.x, o.location.y, z0 + top + 0.006), m='felt', parent=e, cname=cname or name, verts=32)
        box((0.034, 0.008, 0.012), (o.location.x, o.location.y, z0 + top + 0.018), m='chrome', parent=e, cname=cname or name)
        cyl(0.004, 0.05, (o.location.x, o.location.y, z0 + top), m='chrome', parent=e, cname=cname or name, verts=12)
    return e

# ---------- kit ----------
R = math.radians
KIT = {}
KIT['kick'] = drum('kick', 0.279, 0.457, (0, 0, 0.31), (R(90), 0, 0), lugs=10, reso=True, port=True, wood_hoop=True)
KIT['tom1'] = drum('tom1', 0.127, 0.203, (0.165, 0.10, 0.84), (R(-13), R(-7), 0), lugs=6)
KIT['tom2'] = drum('tom2', 0.152, 0.229, (-0.185, 0.10, 0.86), (R(-13), R(8), 0), lugs=6)
KIT['floor'] = drum('floor', 0.203, 0.406, (-0.6, 0.42, 0.40), (R(-2), 0, 0), lugs=8, rows=2)
KIT['snare'] = drum('snare', 0.178, 0.165, (0.36, 0.42, 0.58), (R(-7), R(-3), 0), lugs=10, rows=2, shell='snare_shell')
box((0.03, 0.02, 0.05), (0.36 + 0.19, 0.42, 0.58), m='chrome', cname='snare')

hh = empty('hihat', (0.68, 0.42, 0.0), (0, 0, 0))
cymbal('hihat', 0.178, (0, 0, 0.92), None, felt=False, parent=hh, cname='hihat')
top = cymbal('hihat', 0.178, (0, 0, 0.945), None, felt=False, parent=hh, cname='hihat')
cyl(0.004, 0.25, (0, 0, 1.02), m='chrome', parent=hh, cname='hihat', verts=12)
cyl(0.022, 0.014, (0, 0, 0.985), m='black', parent=hh, cname='hihat', verts=32)

CRASH1 = ((0.34, 0.04, 1.28), (R(-17), R(-10), 0))   # over the rack tom, at crash 2's height
KIT['crash'] = cymbal('crash', 0.229, *CRASH1)
CRASH2 = ((-0.36, 0.06, 1.28), (R(-18), R(8), 0))   # over the rack tom, toward the ride, at crash 1's height
KIT['crash2'] = cymbal('crash2', 0.229, *CRASH2)
KIT['ride'] = cymbal('ride', 0.267, (-0.66, 0.1, 1.06), (R(-12), R(8), 0))
# The china hangs upside down, level front to back and leaning in toward the
# kit's centre. Tilting it toward the drummer like the other cymbals turns it
# edge-on to the camera, and it no longer reads as a china.
CHINA_ROT = (R(180), R(12), 0)
KIT['china'] = cymbal('china', 0.229, (-1.02, 0.36, 1.32), CHINA_ROT, china=True, felt=False)

# ---------- hardware (base layer) ----------
# The hardware belongs to the base layer on the stage. For the slot pictures
# (mode=pieces), each piece is also drawn with its own: its stand, legs or
# spurs; a cymbal with its boom arm only. claim() records which objects those are.
HW = {}

def mark():
    return set(scn.objects)

def claim(piece, before):
    HW.setdefault(piece, []).extend(o for o in scn.objects if o not in before)

# Modelled on real double-braced hardware: tripods with paired leg and brace
# tubes on a hinge casting and sliding collar, telescoping chrome tubes with
# height clamps, memory locks and T-bolts, boom arms with counterweights,
# ratchet tilters, and felts on a threaded rod under each cymbal.
def to_world(loc, rot, p):
    return Vector(loc) + Euler(rot).to_matrix() @ Vector(p)

def ring(p, r, h, axis=(0, 0, 1), m='plastic', verts=32, cname='base'):
    """A short cylinder (clamp, collar, felt) centred on p along axis."""
    o = cyl(r, h, tuple(p), m=m, verts=verts, cname=cname)
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = Vector(axis).to_track_quat('Z', 'Y')
    return o

def slab(a, b, width, thick, m='black'):
    """A flat plate from a to b (footboards, heel plates)."""
    a, b = Vector(a), Vector(b); d = b - a
    o = box((width, d.length, thick), tuple((a + b) / 2), m=m)
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = d.to_track_quat('Y', 'Z')
    return o

def footboard(toe, heel, toe_w, heel_w, thick):
    """A tapered cast footboard from toe to heel, with lightening holes."""
    toe, heel = Vector(toe), Vector(heel); L = (heel - toe).length
    widths = [(0.0, toe_w * 0.8), (0.08, toe_w), (0.45, toe_w * 0.82), (0.85, heel_w), (1.0, heel_w * 0.85)]
    side = [(-w / 2, t * L) for t, w in widths]
    outline = side + [(-x, y) for x, y in reversed(side)]
    n = len(outline)
    verts = [(x, y, z) for z in (thick / 2, -thick / 2) for x, y in outline]
    faces = [tuple(range(n)), tuple(range(2 * n - 1, n - 1, -1))]
    faces += [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    me = bpy.data.meshes.new('footboard'); me.from_pydata(verts, [], faces); me.update()
    o = bpy.data.objects.new('footboard', me); scn.collection.objects.link(o)
    o.location = toe; o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = (heel - toe).to_track_quat('Y', 'Z')
    place(o, None, 'base', 'cast')
    bev = o.modifiers.new('b', 'BEVEL'); bev.width = 0.002; bev.segments = 2
    # Holes: cutters that never render themselves; the boolean modifier still uses them.
    for i, (t, r) in enumerate([(0.18, 0.012), (0.32, 0.009), (0.44, 0.009), (0.56, 0.009), (0.68, 0.008)]):
        for x in ((0.0,) if i == 0 else (-0.017, 0.017)):
            c = cyl(r, 0.05, (0, 0, 0), m=None, verts=24)
            c.parent = o; c.location = (x, t * L, 0); c.hide_render = True
            for col in c.users_collection: col.objects.unlink(c)
            scn.collection.objects.link(c)
            bm = o.modifiers.new('hole', 'BOOLEAN'); bm.operation = 'DIFFERENCE'; bm.object = c
    return o

def t_bolt(p, out):
    """A T-handled clamp bolt sticking out of p."""
    p, out = Vector(p), Vector(out).normalized()
    tip = p + out * 0.034
    tube(p, tip, 0.0035, m='chrome')
    bar = Vector((0, 0, 1)) if abs(out.z) < 0.7 else out.cross(Vector((1, 0, 0))).normalized()
    tube(tip - bar * 0.015, tip + bar * 0.015, 0.0045, m='chrome')

FRONT = (0.45, -1, 0)   # bolts face the camera, as a drummer's tech would set them up for a photo

def tripod(x, y, spread=0.3, hinge=0.34, collar=0.13, facing=90):
    c = Vector((x, y, 0))
    ring((x, y, hinge), 0.024, 0.055, m='chrome')                  # leg hinge casting
    ring((x, y, collar), 0.021, 0.032)                             # sliding brace collar
    t_bolt((x, y, collar), FRONT)
    for i in range(3):
        a = R(facing + i * 120)
        d = Vector((math.cos(a), math.sin(a), 0)); side = Vector((-d.y, d.x, 0))
        top = c + d * 0.026 + Vector((0, 0, hinge - 0.012))
        foot = c + d * spread + Vector((0, 0, 0.018))
        mid = top.lerp(foot, 0.52)
        base = c + d * 0.024 + Vector((0, 0, collar))
        for s in (-1, 1):                                          # double-braced: every leg and brace is a pair
            off = side * (0.0075 * s)
            tube(top + off, foot + off, 0.0052, m='chrome')
            tube(base + off, mid + off, 0.0042, m='chrome')
        blob((0.008, 0.012, 0.008), mid, m='chrome')               # brace pivot
        blob((0.022, 0.022, 0.015), foot - Vector((0, 0, 0.004)), m='rubber')

def column(x, y, stages, start=0.11):
    """Telescoping tube: (top height, radius) per stage, widest first."""
    z = start
    for i, (top, r) in enumerate(stages):
        tube((x, y, z), (x, y, top), r, m='chrome')
        if i < len(stages) - 1:
            ring((x, y, top), r + 0.008, 0.034, m='plastic')        # height clamp
            t_bolt((x, y, top), FRONT)
            ring((x, y, top + 0.024), stages[i + 1][1] + 0.006, 0.012, m='chrome')   # memory lock
        z = top - 0.03
    return Vector((x, y, stages[-1][0]))

def stages_to(height):
    if height < 0.95:
        return [(0.56, 0.0127), (height, 0.0105)]
    return [(0.56, 0.0127), (0.84, 0.0105), (height, 0.009)]

def cymbal_stand(x, y, height, loc, rot, radius, china=False, spread=0.3, facing=90, owner=None):
    tripod(x, y, spread=spread, facing=facing)
    top = column(x, y, stages_to(height))
    boom = mark()   # from here on: the boom, which the cymbal's slot picture shows with it

    axis = Euler(rot).to_matrix() @ Vector((0, 0, 1))
    down = -axis if axis.z > 0 else axis                           # along the cymbal's axis, toward the floor
    hole = to_world(loc, rot, (0, 0, prof(radius, china)[0][1]))
    knuckle = hole + down * 0.085

    tube(knuckle, hole - down * 0.03, 0.004, m='chrome')           # threaded cymbal rod
    ring(hole + down * 0.03, 0.006, 0.03, axis=down, m='plastic')  # sleeve
    ring(hole + down * 0.012, 0.018, 0.012, axis=down, m='felt')   # bottom felt
    ring(hole + down * 0.024, 0.022, 0.003, axis=down, m='chrome') # cup washer
    if china:   # the china's own layer has no top felt; these sit on the cymbal, so they belong to it
        ring(hole - down * 0.012, 0.018, 0.012, axis=down, m='felt', cname='china')
        ring(hole - down * 0.024, 0.01, 0.012, axis=down, m='chrome', cname='china')

    pivot = top + Vector((0, 0, 0.03))
    arm = (knuckle - pivot).normalized()
    side = arm.cross(Vector((0, 0, 1))).normalized()
    ring(pivot, 0.024, 0.04, axis=side, m='chrome')                # boom tilter (ratchet)
    t_bolt(pivot + side * 0.02, side)
    ring(knuckle, 0.016, 0.03, axis=side, m='chrome')              # cymbal tilter (ratchet)
    t_bolt(knuckle + side * 0.015, side)
    tube(pivot - arm * 0.17, knuckle - arm * 0.012, 0.0085, m='chrome')   # boom arm
    ring(pivot - arm * 0.19, 0.017, 0.06, axis=arm, m='chrome')    # counterweight
    if owner:
        claim(owner, boom)

# Cymbal stands (positions chosen so the booms reach in over the kit).
cymbal_stand(0.66, -0.20, 1.06, *CRASH1, 0.229, owner='crash')
# In the gap between the kick and the floor tom, in front of the kit.
cymbal_stand(-0.62, -0.30, 1.06, *CRASH2, 0.229, facing=90, owner='crash2')
cymbal_stand(-0.98, 0.02, 0.86, (-0.66, 0.1, 1.06), (R(-12), R(8), 0), 0.267, facing=150, owner='ride')
cymbal_stand(-1.30, 0.50, 1.02, (-1.02, 0.36, 1.32), CHINA_ROT, 0.229, china=True, owner='china')

# Hi-hat: the tripod is turned so no leg hides the chain from the camera,
# with the pedal between two of its legs.
# The footboard's toe sits right under the stand; a chain runs straight up
# from it to the pull rod that comes out of the bottom of the tube.
hx, hy = 0.68, 0.42
tripod(hx, hy, spread=0.28, hinge=0.36, collar=0.21, facing=300)
column(hx, hy, [(0.62, 0.0127), (0.925, 0.011)], start=0.19)
ring((hx, hy, 0.93), 0.028, 0.012, m='felt')                       # bottom cymbal seat
tube((hx + 0.032, hy + 0.01, 0.23), (hx + 0.032, hy + 0.01, 0.35), 0.009, m='chrome')   # spring housing
blob((0.012, 0.012, 0.012), (hx + 0.032, hy + 0.01, 0.35), m='chrome')

# Pedal frame: two uprights from the bottom of the stand tube down to a floor
# bar, with the chain hanging between them. A link rod runs along the floor
# from the frame to the heel plate, which the footboard is hinged to.
for sx in (-1, 1):
    box((0.012, 0.018, 0.2), (hx + sx * 0.045, hy, 0.112), m='black')
box((0.104, 0.022, 0.016), (hx, hy, 0.212), m='black')              # top bar, clamped to the tube
box((0.104, 0.03, 0.012), (hx, hy, 0.008), m='black')               # floor bar

toe, heel = Vector((hx, hy - 0.006, 0.07)), Vector((hx - 0.03, hy + 0.32, 0.03))
footboard(toe, heel, 0.085, 0.06, 0.012)
box((0.03, 0.022, 0.016), tuple(toe + Vector((0, 0.004, 0.012))), m='chrome')                     # toe clamp
slab(heel + Vector((0, -0.01, -0.018)), heel + Vector((0, 0.07, -0.018)), 0.07, 0.016, m='cast')    # heel plate
tube(heel + Vector((0, 0.01, 0.002)), heel + Vector((0, 0.01, 0.002)) + Vector((0.012, 0, 0)), 0.006, m='chrome')   # hinge pin
tube((hx, hy + 0.012, 0.012), heel + Vector((0, -0.005, -0.015)), 0.0035, m='chrome')               # link rod to the heel plate

tube((hx, hy, 0.2), (hx, hy, 0.168), 0.004, m='chrome')            # pull rod
box((0.016, 0.016, 0.024), (hx, hy, 0.158), m='chrome')            # rod connector
pitch, links = 0.0125, 5
for i in range(links):                                             # chain: alternating links at right angles
    l = torus(0.0055, 0.0019, (hx, hy, 0.143 - i * pitch), scale=(1, 1.6, 1), m='chrome')
    l.rotation_euler = (R(90), 0, R(90) * (i % 2))

# Snare: basket stand gripping the bottom hoop.
m = mark()
SN_LOC, SN_ROT, SN_R, SN_D = (0.36, 0.42, 0.58), (R(-7), R(-3), 0), 0.178, 0.165
tripod(0.36, 0.42, spread=0.24, hinge=0.27, collar=0.11)
column(0.36, 0.42, [(0.36, 0.0127), (0.42, 0.0105)])
blob((0.019, 0.019, 0.019), (0.36, 0.42, 0.44), m='chrome')        # tilter
t_bolt((0.36, 0.42, 0.44), FRONT)
hub = Vector((0.36, 0.42, 0.462))
ring(hub, 0.024, 0.016, m='chrome')
for i in range(3):
    a = R(30 + i * 120)
    tip = to_world(SN_LOC, SN_ROT, ((SN_R + 0.005) * math.cos(a), (SN_R + 0.005) * math.sin(a), -SN_D / 2 - 0.003))
    elbow = Vector((hub.x + (tip.x - hub.x) * 0.92, hub.y + (tip.y - hub.y) * 0.92, tip.z - 0.03))
    tube(hub, elbow, 0.0055, m='chrome')
    tube(elbow, tip, 0.0055, m='chrome')
    blob((0.009, 0.009, 0.012), tip, m='rubber')

claim('snare', m)

# Tom holder: a bracket on the kick shell, a post, and a double holder whose
# L-arms go up into a bracket on each tom.
holder = Vector((0, 0.06, 0.72))
box((0.04, 0.08, 0.018), (0, 0.06, 0.595), m='chrome')
t_bolt((0.03, 0.06, 0.6), FRONT)
tube((0, 0.06, 0.59), holder, 0.0127, m='chrome')
ring((0, 0.06, 0.665), 0.019, 0.016, m='chrome')                   # memory lock
box((0.075, 0.04, 0.032), tuple(holder), m='plastic')
for loc, rot, r, a, sx in (((0.165, 0.10, 0.84), (R(-13), R(-7), 0), 0.127, 135, 1),
                           ((-0.185, 0.10, 0.86), (R(-13), R(8), 0), 0.152, 45, -1)):
    a = R(a)
    bracket = to_world(loc, rot, ((r + 0.02) * math.cos(a), (r + 0.02) * math.sin(a), -0.01))
    knuckle = holder + Vector((sx * 0.055, 0, 0.004))
    ring(knuckle, 0.017, 0.028, axis=(1, 0, 0), m='chrome')        # ratchet tilter
    elbow = Vector((bracket.x, bracket.y, knuckle.z))
    tube(knuckle, elbow, 0.0095, m='chrome')
    blob((0.0095, 0.0095, 0.0095), elbow, m='chrome')
    tube(elbow, bracket, 0.0095, m='chrome')
    box((0.03, 0.03, 0.06), tuple(bracket), m='chrome')
    ring(bracket - Vector((0, 0, 0.038)), 0.014, 0.012, m='chrome')   # memory lock
    t_bolt(bracket, (sx, -0.3, 0))

# Floor tom legs, through brackets on the shell.
m = mark()
FT_LOC, FT_ROT, FT_R = (-0.6, 0.42, 0.40), (R(-2), 0, 0), 0.203
for deg in (210, 330, 90):
    a = R(deg); d = Vector((math.cos(a), math.sin(a), 0))
    bracket = to_world(FT_LOC, FT_ROT, ((FT_R + 0.016) * math.cos(a), (FT_R + 0.016) * math.sin(a), 0.07))
    o = box((0.03, 0.032, 0.05), tuple(bracket), m='chrome'); o.rotation_euler = (0, 0, a)
    t_bolt(bracket + d * 0.015, d)
    top = bracket + d * 0.024 + Vector((0, 0, 0.05))
    foot = Vector((FT_LOC[0], FT_LOC[1], 0.018)) + d * (FT_R + 0.1)
    tube(top, foot, 0.0064, m='chrome')
    blob((0.0075, 0.0075, 0.006), top, m='chrome')
    blob((0.016, 0.016, 0.013), foot - Vector((0, 0, 0.004)), m='rubber')

claim('floor', m)

# Kick spurs, on brackets low on the shell, angled forward and out.
m = mark()
for sx in (1, -1):
    a = R(28)
    bracket = Vector((sx * 0.293 * math.cos(a), -0.11, 0.31 - 0.293 * math.sin(a)))
    box((0.03, 0.06, 0.045), tuple(bracket), m='chrome')
    t_bolt(bracket, (sx, -0.25, 0.2))
    foot = Vector((sx * 0.34, -0.30, 0.016))                       # just outside the shell and a little forward: steep, as real spurs stand
    d = (foot - bracket).normalized()
    tube(bracket - d * 0.05, foot, 0.0085, m='chrome')
    blob((0.016, 0.016, 0.013), foot - Vector((0, 0, 0.004)), m='rubber')

claim('kick', m)

# ---------- extra percussion (slot pictures only) ----------
# Percussion the stage kit doesn't have, modelled only for the Kit tab's slot
# cards (mode=pieces). They're hidden in every other mode, so the stage and
# its layers don't change. Placed in front of the kit, toward the camera.
def mesh_object(name, verts, faces, cname, m):
    me = bpy.data.meshes.new(name); me.from_pydata(verts, [], faces); me.update()
    o = bpy.data.objects.new(name, me); scn.collection.objects.link(o)
    place(o, None, cname, m); smooth(o)
    return o

# Cowbell: tapered black steel, the sides bowing out a little toward the
# mouth, with a mounting clamp; turned three quarters to the camera with its
# mouth tipped up, so you look into it.
cb = bpy.data.objects.new('cowbell', None); cb.location = (0.0, -1.0, 0.7); cb.rotation_euler = (R(24), 0, R(-32))
scn.collection.objects.link(cb); coll('cowbell').objects.link(cb); scn.collection.objects.unlink(cb)
# Rings of the bell from the mouth to the closed back: (y, width, height).
rings = [(-0.085, 0.112, 0.07), (-0.05, 0.096, 0.061), (0.0, 0.08, 0.052), (0.05, 0.068, 0.045), (0.085, 0.06, 0.04)]
verts, faces = [], []
for y, w, h in rings:
    verts += [(sx * w / 2, y, sz * h / 2) for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
for r in range(len(rings) - 1):
    a, b = r * 4, (r + 1) * 4
    faces += [(a + i, a + (i + 1) % 4, b + (i + 1) % 4, b + i) for i in range(4)]
faces.append(tuple(range(len(verts) - 4, len(verts))))           # the closed back; the mouth stays open
bell = mesh_object('cowbell_body', verts, faces, 'cowbell', 'black')
bell.parent = cb
sol = bell.modifiers.new('s', 'SOLIDIFY'); sol.thickness = 0.003
bev = bell.modifiers.new('b', 'BEVEL'); bev.width = 0.008; bev.segments = 4
sub = bell.modifiers.new('sub', 'SUBSURF'); sub.levels = 1; sub.render_levels = 2
box((0.03, 0.05, 0.016), (0, 0.045, 0.03), m='chrome', parent=cb, cname='cowbell')                         # clamp
cyl(0.0055, 0.09, (0, 0.045, 0.078), m='chrome', parent=cb, cname='cowbell', verts=16)                      # mounting rod

# Bell chime: a single brass chime tube, hanging by its cord from a small
# bracket on a mounting rod, turned a little so it reads as round.
ch = bpy.data.objects.new('chimes', None); ch.location = (0.0, -1.0, 0.95); ch.rotation_euler = (0, 0, R(-22))
scn.collection.objects.link(ch); coll('chimes').objects.link(ch); scn.collection.objects.unlink(ch)
CHIME_L = 0.30
cyl(0.0055, 0.16, (-0.09, 0, 0.1), m='chrome', parent=ch, cname='chimes', verts=16)                        # mounting rod
box((0.03, 0.03, 0.026), (-0.09, 0, 0.013), m='chrome', parent=ch, cname='chimes')                          # clamp
box((0.1, 0.012, 0.01), (-0.035, 0, 0.005), m='black', parent=ch, cname='chimes')                           # bracket arm
cyl(0.0022, 0.03, (0.0, 0, -0.012), m='cast', parent=ch, cname='chimes', verts=8)                           # cord
cyl(0.014, CHIME_L, (0.0, 0, -0.027 - CHIME_L / 2), m='brass', parent=ch, cname='chimes', verts=32)         # the chime
torus(0.014, 0.0018, (0.0, 0, -0.027), m='brass', parent=ch, cname='chimes')                                # rolled top edge

# Splash: a 10" cymbal (the crashes are 18"), on a boom like the crashes'.
# Its stand's tripod and column are in the base collection, so everything
# made here is remembered and hidden outside mode=pieces.
before_splash = mark()
SPLASH = ((0.0, -0.55, 1.2), (R(-17), R(-6), 0))
KIT['splash'] = cymbal('splash', 0.127, *SPLASH)
cymbal_stand(0.28, -0.8, 1.0, *SPLASH, 0.127, owner='splash')
splash_objects = [o for o in scn.objects if o not in before_splash]

EXTRAS = ('cowbell', 'chimes', 'splash')
if MODE != 'pieces':
    for name in EXTRAS:
        for o in COLL[name].all_objects:
            o.hide_render = True
    for o in splash_objects:
        o.hide_render = True

bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
floor = bpy.context.object; floor.is_shadow_catcher = True; place(floor, None, 'base', None)

# ---------- lights + camera ----------
def area(loc, energy, size, color=(1, 1, 1)):
    ld = bpy.data.lights.new('A', 'AREA'); ld.energy = energy; ld.size = size; ld.color = color
    o = bpy.data.objects.new('A', ld); o.location = loc
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = (Vector((0, 0.2, 0.7)) - Vector(loc)).to_track_quat('-Z', 'Y')
    scn.collection.objects.link(o)
area((-1.6, -2.2, 3.0), 520, 2.2)
area((2.2, -1.6, 1.8), 160, 1.6, (0.85, 0.92, 1.0))
area((0.4, 2.4, 2.6), 650, 1.4, (1.0, 0.95, 0.88))

# A small warm light inside the kick, behind the front head, so the wood
# inside shows through the port as it does in studio photos. The shell
# encloses it, so it lights nothing else.
ld = bpy.data.lights.new('KickInside', 'POINT'); ld.energy = 6.0; ld.shadow_soft_size = 0.05; ld.color = (1.0, 0.9, 0.78)
o = bpy.data.objects.new('KickInside', ld); o.location = (0.05, -0.12, 0.36)
scn.collection.objects.link(o)

cd = bpy.data.cameras.new('C'); cd.lens = 31
cam = bpy.data.objects.new('C', cd); scn.collection.objects.link(cam); scn.camera = cam
cam.location = (-0.12, -3.4, 2.05)
tgt = Vector((-0.12, 0.25, 0.74))
cam.rotation_mode = 'QUATERNION'; cam.rotation_quaternion = (tgt - cam.location).to_track_quat('-Z', 'Y')

def render(path):
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)

if MODE == 'preview':
    render(os.path.join(OUT, 'preview.png'))
elif MODE == 'pieces':
    # Each piece on its own, nothing in front of it and no floor: the
    # pictures on the Kit tab's slot cards. Same camera and light as the
    # stage, so they look like the kit. (The stage layers can't be used for
    # this: each has holes where the pieces in front of it are.)
    pieces = ['kick', 'tom1', 'tom2', 'floor', 'snare', 'hihat', 'crash', 'crash2', 'ride', 'china', *EXTRAS]
    if 'only' in args:
        pieces = [L for L in pieces if L in args['only'].split(',')]
    every = [o for o in scn.objects if o.type in ('MESH',)]
    hidden = {o for o in every if o.hide_render}   # boolean cutters never render
    def show_only(objs):
        for o in every:
            o.is_holdout = False
            o.hide_render = o in hidden or o not in objs
    for L in pieces:
        # A cymbal (or the snare) is also rendered bare, so crop-layers.py knows where the
        # piece ends and can fade its boom arm (or stand) out from there.
        if L in ('crash', 'crash2', 'ride', 'china', 'splash', 'snare'):
            show_only(set(COLL[L].all_objects))
            render(os.path.join(OUT, 'pieces', L + '-bare.png'))
        show_only(set(COLL[L].all_objects) | set(HW.get(L, [])))
        render(os.path.join(OUT, 'pieces', L + '.png'))
        print('PIECE', L, flush=True)
else:
    layers = ['base', 'kick', 'tom1', 'tom2', 'floor', 'snare', 'hihat', 'crash', 'crash2', 'ride', 'china']
    # only=base,crash2 re-renders just those layers (the others keep their renders).
    if 'only' in args:
        layers = [L for L in layers if L in args['only'].split(',')]
    every = [o for o in scn.objects if o.type in ('MESH',)]
    for L in layers:
        mine = set(COLL[L].all_objects)
        for o in every:
            o.is_holdout = (o not in mine) and not o.is_shadow_catcher
            o.visible_shadow = True
        floor.hide_render = (L != 'base')
        render(os.path.join(OUT, 'layers', L + '.png'))
        print('LAYER', L, flush=True)

if MODE == 'final':
    render(os.path.join(OUT, 'full.png'))
    layers = ['base', 'kick', 'tom1', 'tom2', 'floor', 'snare', 'hihat', 'crash', 'crash2', 'ride', 'china']
    pal = {}
    for i, L in enumerate(layers):
        c = ((i * 37 % 10) / 9.0, (i * 53 % 10) / 9.0, ((i * 71) % 10) / 9.0)
        m = bpy.data.materials.new('id_' + L); m.use_nodes = True
        nt = m.node_tree; nt.nodes.clear()
        em = nt.nodes.new('ShaderNodeEmission'); em.inputs['Color'].default_value = (*c, 1)
        out = nt.nodes.new('ShaderNodeOutputMaterial'); nt.links.new(em.outputs[0], out.inputs['Surface'])
        pal[L] = c
        for o in COLL[L].all_objects:
            if o.type == 'MESH':
                o.data.materials.clear(); o.data.materials.append(m)
    floor.hide_render = True
    scn.world = None
    for o in scn.objects:
        if o.type == 'LIGHT': o.hide_render = True
    scn.view_settings.view_transform = 'Standard'; scn.view_settings.look = 'None'; scn.view_settings.exposure = 0
    scn.cycles.samples = 1; scn.cycles.use_denoising = False
    scn.cycles.filter_width = 0.01
    render(os.path.join(OUT, 'ids.png'))
    json.dump(pal, open(os.path.join(OUT, 'pal.json'), 'w'))
