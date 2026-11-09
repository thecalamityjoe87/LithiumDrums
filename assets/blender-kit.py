import bpy, bmesh, math, sys, os, json
from mathutils import Vector, Euler, Matrix
from mathutils.bvhtree import BVHTree

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
    'rope': mat('rope', (0.30, 0.20, 0.10), rough=0.95),                # natural fibre
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
# A rope-tensioned kit: the kick and toms have a net of cord over the shell
# instead of lugs (the snare keeps its lugs).
ROPE_NET = 'ropeNet' in KIT_FINISH
if ROPE_NET:
    M['rope_net'] = mat('rope_net', tuple(KIT_FINISH['ropeNet']['color']), rough=0.85)

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

def rope_net(name, e, r, d, n, turn, hoop_r, hoop_z, hoop_top):
    """A rope drum's net, as on the Salamander kit: n columns of ties round
    the shell, a row near each hoop. Each tie is an X of cord: two legs up
    over the hoop, two short legs in toward the middle of the shell, where
    they stop, so an open band runs round the middle between the two rows.
    Each cord follows the shell's curve (hoop_r: the hoop's outer radius;
    hoop_z and hoop_top: its middle and its top edge, measured out from the
    shell's end)."""
    rr = r + 0.0025                                      # a cord lies on the shell
    tie_in, leg_in = 0.28 * d, 0.14 * d                  # how far in from the end the ties sit, and the inner legs reach past them
    def at(phase, z, radius=rr):
        a = 2 * math.pi * (phase + turn) / n
        return (radius * math.cos(a), radius * math.sin(a), z)
    def along(p0, z0, p1, z1, steps=14):
        return [at(p0 + (p1 - p0) * t / steps, z0 + (z1 - z0) * t / steps) for t in range(steps + 1)]
    cords, ties = [], []
    for k in range(n):
        c = k + 0.5                                      # this column of ties
        for side in (1, -1):
            z_end, z_tie = side * d / 2, side * (d / 2 - tie_in)
            ties.append((c, z_tie))
            for dp in (-0.35, 0.35):
                # Up: from over the hoop, down the shell to the tie.
                hook = [at(c + dp, side * (d / 2 + hoop_top + 0.002), r + 0.004), at(c + dp, side * (d / 2 + hoop_z), hoop_r + 0.002)]
                cords.append(hook + along(c + dp, z_end - side * 0.01, c, z_tie))
                # In: a short leg toward the middle, ending in the open band.
                cords.append(along(c, z_tie, c + dp * 0.9, z_tie - side * leg_in, steps=8))
    cu = bpy.data.curves.new(name + '_net', 'CURVE'); cu.dimensions = '3D'
    cu.bevel_depth = 0.0018; cu.bevel_resolution = 2
    for pts in cords:
        sp = cu.splines.new('POLY'); sp.points.add(len(pts) - 1)
        for p, co in zip(sp.points, pts): p.co = (*co, 1.0)
    ob = bpy.data.objects.new(name + '_net', cu); scn.collection.objects.link(ob)
    # As a mesh, like everything else (the render modes only show and hide meshes).
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(bpy.context.evaluated_depsgraph_get()))
    bpy.data.objects.remove(ob)
    net = bpy.data.objects.new(name + '_net', me); scn.collection.objects.link(net); smooth(net)
    place(net, e, name, 'rope_net')
    # The tie at each X: a few turns of cord binding its legs together.
    for ph, z in ties:
        blob((0.0045, 0.0045, 0.009), at(ph, z, rr + 0.001), m='rope_net', parent=e, cname=name)

def drum(name, r, d, loc, rot, lugs, rows=1, shell='shell', reso=False, port=False, wood_hoop=False, lug_turn=0.5):
    # lug_turn: where the lugs start, in lug spacings from the drum's local x axis.
    roped = ROPE_NET and shell != 'snare_shell'   # a rope drum: a net instead of lugs
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
    if roped:
        if wood_hoop:
            rope_net(name, e, r, d, lugs, lug_turn, r + 0.016, -0.006, 0.009)       # the wooden hoop and its chrome band
        else:
            rope_net(name, e, r, d, lugs, lug_turn, r + 0.0095, 0.004, 0.0145)
    for i in range(0 if roped else lugs):
        a = 2 * math.pi * (i + lug_turn) / lugs
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
        for i in range(0 if roped else lugs):
            ang = 2 * math.pi * (i + lug_turn) / lugs
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
# The kick's lugs start on its side (lug_turn=0), so the top is between two
# lugs: the tom holder's bracket sits there, as on a real kick.
KIT['kick'] = drum('kick', 0.279, 0.457, (0, 0, 0.31), (R(90), 0, 0), lugs=10, reso=True, port=True, wood_hoop=True, lug_turn=0)
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
KIT['china'] = cymbal('china', 0.229, (-0.90, 0.36, 1.26), CHINA_ROT, china=True, felt=False)

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
cymbal_stand(-1.17, 0.21, 0.96, (-0.90, 0.36, 1.26), CHINA_ROT, 0.229, china=True, owner='china')   # in front, the boom reaching back like the others'

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
# A wing nut straight out of the bracket's side, square to it, made like the
# cowbell's: stud, hub and two wings. The wings lie nearly flat (tipped 12
# degrees, the front one up toward the camera): upright, the lower one would
# sink into the shell. (The old T-bolt started 1 cm off the side and floated.)
cyl(0.003, 0.016, (0.026, 0.06, 0.598), rot=(0, R(90), 0), m='chrome', verts=12)       # stud,
cyl(0.0058, 0.011, (0.0255, 0.06, 0.598), rot=(0, R(90), 0), m='chrome', verts=16)     # hub
for sy in (-1, 1):
    wing = blob((0.008, 0.0105, 0.0022), (0.028, 0.06 + sy * 0.0115 * math.cos(R(12)), 0.598 - sy * 0.0115 * math.sin(R(12))), m='chrome')
    wing.rotation_euler = (R(-12), 0, 0)                                                 # and wings
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
# mouth, with a mounting clamp on its closed back, held by a wing nut; turned
# three quarters to the camera with its mouth tipped up, so you look into it.
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
box((0.03, 0.02, 0.036), (0, 0.095, 0), m='chrome', parent=cb, cname='cowbell')                             # clamp, on the closed back
cyl(0.0055, 0.17, (0, 0.1, 0.02), rot=(R(-24), 0, 0), m='chrome', parent=cb, cname='cowbell', verts=16)     # mounting rod, upright through the clamp
cyl(0.003, 0.02, (0, 0.115, 0), rot=(R(90), 0, 0), m='chrome', parent=cb, cname='cowbell', verts=12)        # wing nut on the clamp's back: its stud,
cyl(0.0058, 0.011, (0, 0.113, 0), rot=(R(90), 0, 0), m='chrome', parent=cb, cname='cowbell', verts=16)      # hub
for sx in (-1, 1):
    blob((0.0105, 0.008, 0.0022), (sx * 0.0115, 0.117, 0), m='chrome', parent=cb, cname='cowbell')          # and wings

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

# Gong: a 24" bronze chau gong (a raised centre boss, a flat face and a
# turned-back rim) hung by two ropes from the top bar of a black steel frame,
# turned a little so it reads as round. Built in world coordinates, so the
# frame's parts are placed with gong_at().
GONG_BASE, GONG_YAW, GONG_R, GONG_Z = Vector((0.0, 0.1, 0.0)), R(-25), 0.305, 0.62
def gong_at(x, y, z):
    return GONG_BASE + Euler((0, 0, GONG_YAW)).to_matrix() @ Vector((x, y, z))

gong = lathe('gong_face', [(0.001, 0.016), (0.07, 0.016), (0.09, 0.002), (GONG_R * 0.9, 0.0), (GONG_R * 0.97, -0.008), (GONG_R, -0.03), (GONG_R, -0.07)], seg=128)
gong.location = gong_at(0, -0.05, GONG_Z); gong.rotation_euler = (R(90), 0, GONG_YAW)   # the face looks toward the camera; the rim's middle is in the frame's plane
sol = gong.modifiers.new('s', 'SOLIDIFY'); sol.thickness = 0.004
place(gong, None, 'gong', 'bronze')

FRAME_X, FRAME_TOP, FOOT_Y = 0.42, 1.06, 0.3

def rope(a, b, strands=3, lay=0.045, radius=0.0028, strand_r=0.0026, steps=110, sides=10):
    """A laid rope from point a to point b (in the gong's frame): strands
    twisted round each other, one turn every `lay`. A mesh in world coordinates."""
    a, b = Vector(a), Vector(b)
    d = (b - a).normalized(); u = d.cross(Vector((0, 1, 0))).normalized(); v = d.cross(u)
    verts, faces = [], []
    for s in range(strands):
        phase = 2 * math.pi * s / strands
        pts = []
        for i in range(steps + 1):
            t = i / steps
            ang = phase + 2 * math.pi * (b - a).length * t / lay
            pts.append(a + (b - a) * t + radius * (math.cos(ang) * u + math.sin(ang) * v))
        base = len(verts)
        for i, p in enumerate(pts):
            tan = (pts[min(i + 1, steps)] - pts[max(i - 1, 0)]).normalized()
            n1 = tan.cross(Vector((0, 1, 0))).normalized(); n2 = tan.cross(n1)
            for k in range(sides):
                th = 2 * math.pi * k / sides
                verts.append(tuple(gong_at(*(p + strand_r * (math.cos(th) * n1 + math.sin(th) * n2)))))
        for i in range(steps):
            for k in range(sides):
                a0 = base + i * sides + k; a1 = base + i * sides + (k + 1) % sides
                faces.append((a0, a1, a1 + sides, a0 + sides))
    return mesh_object('rope', verts, faces, 'gong', 'rope')

HOLE_R = 0.0065   # a 13 mm hole through each side of the rim, for a rope about 11 mm across
for sx in (-1, 1):
    tube(gong_at(sx * FRAME_X, 0, 0.02), gong_at(sx * FRAME_X, 0, FRAME_TOP), 0.017, m='black', cname='gong')            # upright
    tube(gong_at(sx * FRAME_X, -FOOT_Y, 0.02), gong_at(sx * FRAME_X, FOOT_Y, 0.02), 0.017, m='black', cname='gong')    # foot
    tube(gong_at(sx * FRAME_X, -FOOT_Y * 0.9, 0.025), gong_at(sx * FRAME_X, 0, 0.42), 0.009, m='black', cname='gong')   # braces
    tube(gong_at(sx * FRAME_X, FOOT_Y * 0.9, 0.025), gong_at(sx * FRAME_X, 0, 0.42), 0.009, m='black', cname='gong')
    for sy in (-1, 1):
        blob((0.022, 0.022, 0.016), gong_at(sx * FRAME_X, sy * FOOT_Y, 0.012), m='rubber', cname='gong')                # feet
    # A hole through the rim near the top, cut straight through the flange.
    # The rope goes up from a knot inside the rim, through the hole, to the
    # top bar, and is wrapped a few times round the bar.
    ex = sx * 0.16
    ry = math.sqrt(GONG_R ** 2 - ex ** 2)
    out = Vector((ex / GONG_R, 0, ry / GONG_R))              # straight out from the gong's centre
    hole = Vector((ex, 0, GONG_Z + ry))
    cutter = tube(gong_at(*(hole - out * 0.02)), gong_at(*(hole + out * 0.02)), HOLE_R, m=None, cname='gong')
    cutter.hide_render = True; cutter.hide_viewport = True
    cut = gong.modifiers.new('hole', 'BOOLEAN'); cut.operation = 'DIFFERENCE'; cut.object = cutter; cut.solver = 'EXACT'
    knot = hole - out * 0.014
    rope(knot, (ex, 0, FRAME_TOP - 0.02))
    blob((0.013, 0.011, 0.013), gong_at(*knot), m='rope', cname='gong')
    for i in range(3):
        wrap = torus(0.0206, 0.0036, gong_at(ex + (i - 1) * 0.008, 0, FRAME_TOP), m='rope', cname='gong')
        wrap.rotation_euler = (0, R(90), GONG_YAW)    # the ring's axis along the bar
tube(gong_at(-FRAME_X, 0, FRAME_TOP), gong_at(FRAME_X, 0, FRAME_TOP), 0.017, m='black', cname='gong')                   # top bar
for sx in (-1, 1):
    blob((0.024, 0.024, 0.024), gong_at(sx * FRAME_X, 0, FRAME_TOP), m='black', cname='gong')                            # corner joints

# Splash: a 10" cymbal (the crashes are 18"), low and between the snare and
# the hi-hat, on its own boom stand beside the kit.
SPLASH = ((0.45, 0.3, 0.96), (R(-16), R(-12), 0))
KIT['splash'] = cymbal('splash', 0.127, *SPLASH)
cymbal_stand(0.77, 0.12, 0.85, *SPLASH, 0.127, spread=0.24, owner='splash')

# ---------- more percussion (slot pictures only) ----------
# Like the cowbell and chimes: modelled only for the slot cards, hidden in
# every other mode. Small things are drawn bigger than life so their pictures
# are as detailed as the drums'. Parts are placed in each piece's own frame
# (an empty), so they follow its position and tilt.
M['rosewood'] = mat('rosewood', (0.16, 0.05, 0.025), rough=0.35, coat=0.4)
M['skin'] = mat('skin', (0.5, 0.31, 0.22), rough=0.55)
M['galv'] = mat('galv', (0.58, 0.6, 0.62), metal=1.0, rough=0.34)   # galvanised steel

def frame(name, loc, rot, parent=None):
    e = empty(name, loc, rot)
    if parent: e.parent = parent
    return e

def limb(a, b, r, m, parent, cname, ends=True):
    """A rounded rod from a to b, in the parent's frame (a finger, a handle)."""
    a, b = Vector(a), Vector(b); d = b - a
    o = cyl(r, d.length, (a + b) / 2, m=m, parent=parent, cname=cname, verts=20)
    o.rotation_mode = 'QUATERNION'; o.rotation_quaternion = d.to_track_quat('Z', 'Y')
    if ends:
        for p in (a, b):
            blob((r, r, r), p, m=m, parent=parent, cname=cname)
    return o

def revolve(name, profile, parent, cname, m, thickness=None, seg=64, smooth_levels=0):
    o = lathe(name, profile, seg)
    if thickness: o.modifiers.new('s', 'SOLIDIFY').thickness = thickness
    if smooth_levels:
        sub = o.modifiers.new('sub', 'SUBSURF'); sub.levels = smooth_levels; sub.render_levels = smooth_levels + 1
    return place(o, parent, cname, m)

# Tambourine: a headed tambourine like the usual product shot: a white head
# under a black counter-hoop, a figured wood shell with four slots holding
# pairs of nickel jingles, and a nickel tuning bracket between each slot.
# Tilted up toward the camera so the shell and jingles show.
TAMB_R = 0.15
M['tamb_wood'] = finish_mat('tamb_wood', {'style': 'stain', 'color': [0.16, 0.05, 0.015], 'color2': [0.5, 0.22, 0.07], 'rough': 0.38, 'coat': 0.5})
M['head_white'] = mat('head_white', (0.9, 0.92, 0.95), rough=0.32, coat=0.3)
M['hoop'] = mat('hoop', (0.012, 0.012, 0.014), metal=0.8, rough=0.3)
tb = frame('tambourine', (0.0, -1.0, 0.75), (R(31.8), 0, R(-49.4)))
shell = cyl(TAMB_R, 0.06, (0, 0, 0), m='tamb_wood', parent=tb, cname='tambourine', verts=96, fill='NOTHING')
shell.modifiers.new('s', 'SOLIDIFY').thickness = 0.012
cyl(TAMB_R - 0.004, 0.003, (0, 0, 0.026), m='head_white', parent=tb, cname='tambourine', verts=96)
torus(TAMB_R - 0.001, 0.0065, (0, 0, 0.03), m='hoop', parent=tb, cname='tambourine')                     # the counter-hoop
for k in range(4):
    a = R(45 + 90 * k)
    c = Vector((TAMB_R * math.cos(a), TAMB_R * math.sin(a), -0.004))
    tangent = Vector((-math.sin(a), math.cos(a), 0))
    slot = box((0.13, 0.05, 0.02), tuple(c), rot=(0, 0, a + R(90)), parent=tb, cname='tambourine')
    slot.hide_render = True; slot.hide_viewport = True
    shell.modifiers.new('slot', 'BOOLEAN').object = slot
    shell.modifiers[-1].operation = 'DIFFERENCE'; shell.modifiers[-1].solver = 'EXACT'
    for pair in (-0.032, 0.032):
        for face in (-1, 1):
            j = cyl(0.027, 0.0014, tuple(c + tangent * (pair + face * 0.0045)), rot=(R(90), 0, a), m='chrome', parent=tb, cname='tambourine', verts=32)
            j.rotation_euler = (R(90 + face * 4), 0, a)                                                    # each pair opens a little, like real jingles
        limb(tuple(c + tangent * (pair - 0.012)), tuple(c + tangent * (pair + 0.012)), 0.0022, 'chrome', tb, 'tambourine', ends=False)   # the pin
for k in range(4):                                                                                         # tuning brackets: a hook over the hoop and a strap down the shell
    a = R(90 * k)
    box((0.016, 0.018, 0.0045), (( TAMB_R - 0.003) * math.cos(a), (TAMB_R - 0.003) * math.sin(a), 0.0395), rot=(0, 0, a), m='chrome', parent=tb, cname='tambourine')
    box((0.0035, 0.016, 0.052), ((TAMB_R + 0.0018) * math.cos(a), (TAMB_R + 0.0018) * math.sin(a), 0.008), rot=(0, 0, a), m='chrome', parent=tb, cname='tambourine')
    cyl(0.0042, 0.006, ((TAMB_R + 0.004) * math.cos(a), (TAMB_R + 0.004) * math.sin(a), -0.012), rot=(0, R(90), a), m='chrome', parent=tb, cname='tambourine', verts=16)   # bolt head

# Maracas: a pair of rosewood gourds held up on their handles, leaning apart.
maraca_profile = [(0.0005, 0.0)]
for i in range(1, 15):
    t = i / 14 * 0.85 * math.pi
    maraca_profile.append((0.055 * math.sin(t), 0.056 - 0.056 * math.cos(t)))
maraca_profile += [(0.017, 0.122), (0.0125, 0.138), (0.012, 0.17), (0.0145, 0.186), (0.017, 0.196), (0.016, 0.206), (0.011, 0.214), (0.0005, 0.217)]
for sx in (-1, 1):
    g = frame('maraca', (sx * 0.025, -1.0, 0.62), (R(-12), sx * R(16), 0))    # at the handle's end
    held = frame('maraca', (0, 0, 0.217), (R(180), 0, 0), parent=g)           # the profile runs gourd first, so turn it over
    revolve('maraca', maraca_profile, held, 'maraca', 'rosewood', smooth_levels=1)
    torus(0.0175, 0.0025, (0, 0, 0.13), m='brass', parent=held, cname='maraca')

# Shaker: a glossy plastic egg shaker, leaning, with a seam round its middle.
M['shaker_shell'] = mat('shaker_shell', (0.03, 0.35, 0.42), rough=0.3, coat=0.5)
EGG = [(0.05 * math.sin(t) * (1 + 0.12 * math.cos(t)) + 0.0005, 0.07 - 0.07 * math.cos(t)) for t in (i / 24 * math.pi for i in range(25))]
g = frame('shaker', (0.0, -1.0, 0.7), (R(-28), R(24), 0))
g.scale = (1.4, 1.4, 1.4)
revolve('shaker', EGG, g, 'shaker', 'shaker_shell', smooth_levels=1)
torus(0.0505, 0.0016, (0, 0, 0.07), m='black', parent=g, cname='shaker')   # sunk half into the shell: a seam

# Claves: two rosewood sticks crossed, turned a little toward the camera. The
# ends are cut flat with a small eased edge; the extra rings keep the smooth
# shading from rounding the flat face.
CLAVE_R, CLAVE_HALF = 0.017, 0.15
CLAVE_END = [(0.000001, 0), (CLAVE_R - 0.0024, 0), (CLAVE_R - 0.0012, 0), (CLAVE_R - 0.0002, 0.001), (CLAVE_R, 0.0022), (CLAVE_R, 0.0036)]
CLAVE = [(r, z - CLAVE_HALF) for r, z in CLAVE_END] + [(r, CLAVE_HALF - z) for r, z in reversed(CLAVE_END)]
g = frame('claves', (0.0, -1.0, 0.75), (R(20), 0, R(-20)))
for phi, dy in ((55, 0.0), (125, -0.034)):
    holder = frame('claves', (0, dy, 0), (0, R(phi), 0), parent=g)
    revolve('claves', CLAVE, holder, 'claves', 'rosewood', seg=48)

# Bell tree: twelve brass bells nested on a chrome rod, biggest at the bottom.
g = frame('belltree', (0.0, -1.0, 0.45), (R(-6), 0, R(-18)))
cyl(0.0035, 0.5, (0, 0, 0.25), m='chrome', parent=g, cname='belltree', verts=12)
blob((0.009, 0.009, 0.009), (0, 0, 0.5), m='chrome', parent=g, cname='belltree')
for i in range(12):
    rb = 0.075 - 0.0045 * i; h = 0.62 * rb
    top_z = 0.035 + 0.62 * 0.075 + i * 0.034
    dome = [(rb * f, -h * f * f) for f in (0.04 + 0.96 * j / 9 for j in range(10))]
    bell = revolve('bell', dome, g, 'belltree', 'brass', thickness=0.0025)
    bell.location = (0, 0, top_z)
    torus(rb, 0.0025, (0, 0, top_z - h), m='brass', parent=g, cname='belltree')

# Bucket: a galvanised pail with a rolled rim, ribs, ears and a wire handle.
g = frame('bucket', (0.0, -0.9, 0.45), (0, 0, R(-20)))
revolve('bucket', [(0.001, 0), (0.098, 0), (0.1, 0.004), (0.108, 0.07), (0.118, 0.17), (0.128, 0.26), (0.131, 0.27)], g, 'bucket', 'galv', thickness=0.0025, seg=96)
torus(0.1325, 0.0045, (0, 0, 0.27), m='galv', parent=g, cname='bucket')
for z, r in ((0.1, 0.111), (0.19, 0.1198)):
    torus(r, 0.003, (0, 0, z), m='galv', parent=g, cname='bucket')
handle = [Vector((0.135 * math.cos(t), 0.135 * math.sin(t) * math.sin(R(25)), 0.255 + 0.135 * math.sin(t) * math.cos(R(25)))) for t in (i / 20 * math.pi for i in range(21))]
for a, b in zip(handle, handle[1:]):
    limb(a, b, 0.0035, 'galv', g, 'bucket')
for sx in (-1, 1):
    box((0.012, 0.022, 0.03), (sx * 0.133, 0, 0.255), m='cast', parent=g, cname='bucket')

# Stack: two small cymbals on one rod, face to face, on a boom like the splash's.
before_stack = mark()
STACK = ((0.0, -0.55, 1.1), (R(-12), R(-8), 0))
lower = cymbal('stack', 0.15, *STACK, felt=False)
top_cymbal = frame('stack', (0, 0, 0.045), (R(180), 0, 0), parent=lower)
cymbal('stack', 0.15, (0, 0, 0), None, felt=False, parent=top_cymbal, cname='stack')
KIT['stack'] = lower
cyl(0.004, 0.07, (0, 0, 0.032), m='chrome', parent=lower, cname='stack', verts=12)
cyl(0.018, 0.008, (0, 0, 0.034), m='felt', parent=lower, cname='stack', verts=32)
box((0.036, 0.012, 0.014), (0, 0, 0.062), m='chrome', parent=lower, cname='stack')   # wing nut
cymbal_stand(0.28, -0.8, 0.9, *STACK, 0.15, owner='stack')
stack_objects = [o for o in scn.objects if o not in before_stack]

# Hands and the boot are real models rather than shapes built here. The hands
# are a MakeHuman body (CC0), posed in Blender and cut down to the hand and
# forearm; the boot is Poly Haven's Rubber Boots (CC0). See assets/models/README.md.


HERE = os.path.dirname(os.path.abspath(__file__))
M['skin_real'] = mat('skin_real', (0.5, 0.31, 0.22), rough=0.5)
def _skin_detail():                                      # a little subsurface scatter, so it doesn't look like plastic
    nt = M['skin_real'].node_tree; p = nt.nodes['Principled BSDF']
    p.inputs['Subsurface Weight'].default_value = 0.25
    p.inputs['Subsurface Radius'].default_value = (1.0, 0.4, 0.25)
    p.inputs['Subsurface Scale'].default_value = 0.02
_skin_detail()

def load_hand(pose, part, parent, cname, wrist, fingers, palm, mirror=False):
    """A hand from assets/models (see README there), with its wrist at `wrist`
    in the parent's frame, its fingers pointing along `fingers` and its palm
    facing `palm`. `part` is 'hand' (to the wrist) or 'full' (with the forearm).
    `mirror` makes it the other hand."""
    info = json.load(open(os.path.join(HERE, 'models', 'hand_%s.json' % pose)))
    bpy.ops.wm.obj_import(filepath=os.path.join(HERE, 'models', 'hand_%s_%s.obj' % (pose, part)))
    o = bpy.context.selected_objects[0]
    o.data.transform(o.matrix_world); o.matrix_world = Matrix.Identity(4)   # the importer keeps its axis conversion on the object; bake it into the mesh
    def basis(y, z):
        y = Vector(y).normalized(); z = Vector(z); z = (z - y * z.dot(y)).normalized()
        return Matrix((y.cross(z), y, z)).transposed()
    rotation = basis(fingers, palm) @ basis(info['finger_dir'], info['palm_dir']).transposed()
    if mirror:                                           # reflect across the plane that holds the fingers and the palm's normal
        s = basis(fingers, palm).col[0]
        rotation = (Matrix.Identity(3) - 2 * Matrix([[s[i] * s[j] for j in range(3)] for i in range(3)])) @ rotation
    o.data.transform(Matrix.Translation(wrist) @ rotation.to_4x4() @ Matrix.Translation(-Vector(info['wrist'])))
    o.matrix_world = Matrix.Identity(4)
    if mirror:
        bm = bmesh.new(); bm.from_mesh(o.data); bmesh.ops.reverse_faces(bm, faces=bm.faces); bm.to_mesh(o.data); bm.free()
    smooth(o)
    sub = o.modifiers.new('sub', 'SUBSURF'); sub.levels = 1; sub.render_levels = 2
    return place(o, parent, cname, 'skin_real')

# Hand clap: two bare hands meeting at an angle, as in a photo of a clap. The
# front hand is seen from the back, fingers up and to the left; the one behind
# shows its palm, fingers pointing left. The forearms are kept in their own
# collection: they're drawn with the picture but not counted when cropping, so
# crop-layers.py fades them out.
g = frame('clap', (0.0, -1.0, 0.75), (0, 0, 0))
# Right hand in front, back toward the camera, fingers up and a little to the right; the left hand behind it,
# palm toward the camera, angled further right. Both wrists at the bottom.
CLAP_A = ((-0.02, -0.019, 0.0), (math.sin(R(25)), 0, math.cos(R(25))), (0, 1, 0), False)
CLAP_B = ((0.0, 0.019, -0.012), (math.sin(R(48)), 0, math.cos(R(48))), (0, -1, 0), True)
clap_a = [load_hand('flat', part, g, cn, *CLAP_A[:3], mirror=CLAP_A[3]) for part, cn in (('hand', 'clap'), ('full', 'clap_arms'))]
clap_b = [load_hand('flat', part, g, cn, *CLAP_B[:3], mirror=CLAP_B[3]) for part, cn in (('hand', 'clap'), ('full', 'clap_arms'))]
def _overlaps(a, b, dy):
    ta = BVHTree.FromPolygons([tuple(v.co) for v in a.data.vertices], [tuple(p.vertices) for p in a.data.polygons])
    tb = BVHTree.FromPolygons([tuple(v.co + Vector((0, dy, 0))) for v in b.data.vertices], [tuple(p.vertices) for p in b.data.polygons])
    return bool(ta.overlap(tb))
# Slide the left hand toward the camera-facing side until the hands just stop touching, so no finger goes through the other hand.
dy = 0.03
while dy > -0.03 and not _overlaps(clap_a[0], clap_b[0], dy):
    dy -= 0.001
dy += 0.0025
for o in clap_b:
    o.data.transform(Matrix.Translation((0, dy, 0)))
HW['clap'] = list(COLL['clap_arms'].all_objects)

# Finger snaps, as in the photo: the palm faces up and toward the camera, the fingers
# curl in with the middle finger pressed to the thumb, and the forearm runs off to the lower right.
g = frame('snap', (0.0, -1.0, 0.82), (0, 0, 0))
for part, cn in (('hand', 'snap'), ('full', 'snap_arms')):
    load_hand('snap', part, g, cn, (0.0, 0.0, 0.0), (-0.8796, 0.2511, 0.4041), (0.2839, -0.4046, 0.8693))
HW['snap'] = list(COLL['snap_arms'].all_objects)

# Foot stomp: Poly Haven's rubber boot (CC0), made into a black lace-up ankle
# boot, with its heel raised mid-stomp on a plywood stomp board. The boot pivots
# on its toe, which stays on the board.
g = frame('stomp', (0.0, -0.85, 0.5), (0, 0, R(-35)))
box((0.55, 0.36, 0.03), (0, 0, -0.015), m='wood', parent=g, cname='stomp')
foot = frame('stomp', (0.12, 0, 0.0), (0, R(18), 0), parent=g)
before_boot = set(scn.objects)
bpy.ops.import_scene.gltf(filepath=os.path.join(HERE, 'models', 'rubber_boots', 'rubber_boots_1k.gltf'))
imported = [o for o in scn.objects if o not in before_boot]
boot = next(o for o in imported if o.name == 'rubber_boots_r')   # the pack has a clean and a dirty pair; one boot is enough
for o in imported:
    if o is not boot: bpy.data.objects.remove(o)
turn = Matrix.Rotation(R(90), 4, 'Z')                             # toe toward +x, as the frame expects
corners = [turn @ boot.matrix_world @ Vector(c) for c in boot.bound_box]
boot.data.transform(Matrix.Translation((-max(c.x for c in corners), -(min(c.y for c in corners) + max(c.y for c in corners)) / 2, -min(c.z for c in corners))) @ turn @ boot.matrix_world)
boot.matrix_world = Matrix.Identity(4)                            # the toe's tip, on the ground, is now the origin
place(boot, foot, 'stomp', None)
for mt in boot.data.materials:                                    # plain black leather: the colour texture is what carries the green and the logo
    nt = mt.node_tree
    bsdf = next(n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED')
    for link in list(bsdf.inputs['Base Color'].links):
        nt.links.remove(link)
    bsdf.inputs['Base Color'].default_value = (0.012, 0.011, 0.012, 1)
    bsdf.inputs['Coat Weight'].default_value = 0.35
M['boot_black'] = mat('boot_black', (0.012, 0.011, 0.012), rough=0.3, coat=0.4)
M['lace'] = mat('lace', (0.82, 0.8, 0.72), rough=0.8)

def boot_surface(x=None, z=None, y=0.0):
    """The boot's outer surface above a point on the vamp (given x) or in front of a point on the shaft (given z), at width y."""
    for reach in (0.006, 0.012, 0.02, 0.03):                      # widen the search until it finds the surface
        near = [v.co for v in boot.data.vertices if abs(v.co.y - y) < reach and (abs(v.co.x - x) < reach if z is None else abs(v.co.z - z) < reach)]
        if near: break
    if z is None:
        top = max(near, key=lambda p: p.z)
        return Vector((x, y, top.z))
    front = max(near, key=lambda p: p.x)
    return Vector((front.x, y, z))

LACING = [dict(x=-0.07), dict(x=-0.09), dict(x=-0.105), dict(x=-0.118), dict(z=0.13), dict(z=0.15), dict(z=0.17), dict(z=0.19)]
def lace_at(i, y):
    return boot_surface(y=y, **LACING[i]) + Vector((0, 0, 0.0015)) if 'x' in LACING[i] else boot_surface(y=y, **LACING[i]) + Vector((0.0015, 0, 0))
for i in range(len(LACING) - 1):                                  # laces crossing over the front
    limb(lace_at(i, -0.022), lace_at(i + 1, 0.022), 0.0021, 'lace', foot, 'stomp')
    limb(lace_at(i, 0.022), lace_at(i + 1, -0.022), 0.0021, 'lace', foot, 'stomp')
for side in (-1, 1):                                              # the loose ends, hanging down the sides
    top = lace_at(len(LACING) - 1, side * 0.022)
    limb(top, top + Vector((0.004, side * 0.014, -0.05)), 0.0021, 'lace', foot, 'stomp')

# An ankle boot, not a wellington: cut the tall shaft down.
CUT = 0.205
bm = bmesh.new(); bm.from_mesh(boot.data)
bmesh.ops.bisect_plane(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces), plane_co=(0, 0, CUT), plane_no=(0, 0, 1), clear_outer=True)
bm.to_mesh(boot.data); bm.free()

# Stick click: two drumsticks crossed, tip to tip.
STICK = [(0.0005, 0), (0.0062, 0.004), (0.0068, 0.01), (0.0072, 0.15), (0.0078, 0.25), (0.0062, 0.33), (0.0042, 0.375), (0.0058, 0.39), (0.0062, 0.4), (0.005, 0.41), (0.0005, 0.415)]
g = frame('stickclick', (0.0, -1.0, 0.75), (R(20), 0, R(-20)))
for phi, dy in ((55, 0.0), (125, -0.03)):
    holder = frame('stickclick', (0, dy, 0), (0, R(phi), 0), parent=g)
    stick = revolve('stick', STICK, holder, 'stickclick', 'wood', smooth_levels=1)
    stick.location = (0, 0, -0.2)    # crossed at the middle of each stick

# Cajon: a maple box with a lighter plywood playing face, screwed on at the
# sides and bottom and free at the top edge, on rubber feet. The Cobblestone
# kit's Cajon and Cajon Slap both use this picture.
M['tapa'] = mat('tapa', (0.85, 0.62, 0.38), rough=0.55)
g = frame('cajon', (0.0, -0.9, 0.5), (R(8), 0, R(-30)))
body = box((0.3, 0.3, 0.48), (0, 0, 0.26), m='wood', parent=g, cname='cajon')
body.modifiers['b'].width = 0.005; body.modifiers['b'].segments = 2
face = box((0.3, 0.008, 0.46), (0, -0.154, 0.255), m='tapa', parent=g, cname='cajon')
face.modifiers['b'].width = 0.002
box((0.296, 0.004, 0.004), (0, -0.159, 0.488), m='black', parent=g, cname='cajon')          # the free top edge
for k in range(9):
    for x, z in ((-0.14 + k * 0.035, 0.03), (-0.14, 0.03 + k * 0.05), (0.14, 0.03 + k * 0.05)):
        cyl(0.0045, 0.003, (x, -0.1595, z), rot=(R(90), 0, 0), m='chrome', parent=g, cname='cajon', verts=12)
for sx in (-1, 1):
    for sy in (-1, 1):
        blob((0.022, 0.022, 0.012), (sx * 0.12, sy * 0.12, 0.012), m='rubber', parent=g, cname='cajon')

# Congas: a small and a large one, lacquered wood staves with a skin head, a
# single hoop, and six short hooks down to brackets on the upper shell. The body
# below the brackets is bare.
M['conga'] = mat('conga', (0.42, 0.15, 0.045), rough=0.32, coat=0.55)
M['conga_hw'] = mat('conga_hw', (0.012, 0.012, 0.014), metal=0.5, rough=0.35)
def conga(name, loc, rot, height, top_r, belly_r, foot_r):
    g = frame(name, loc, rot)
    def radius_at(s):                                    # s: 0 at the foot, 1 at the head
        if s <= 0.7:
            return foot_r + (belly_r - foot_r) * math.sin(math.pi / 2 * s / 0.7)
        return belly_r - (belly_r - top_r) * ((s - 0.7) / 0.3) ** 1.5
    profile = [(radius_at(i / 24), height * i / 24) for i in range(25)]
    WALL = 0.008                                         # lathe faces point inward, so the wall grows outward: the outer surface is radius_at() + WALL
    revolve(name, profile, g, name, 'conga', thickness=WALL, seg=96)
    cyl(top_r + 0.004, 0.005, (0, 0, height + 0.002), m='head', parent=g, cname=name, verts=96)
    torus(top_r + 0.006, 0.008, (0, 0, height), m='conga_hw', parent=g, cname=name)   # the only hoop: it holds the head
    outer = lambda s: radius_at(s) + WALL
    for k in range(6):                                   # six tuning points: a hook from the hoop down to a bracket on the shell
        a = R(30 + 60 * k)
        c, s = math.cos(a), math.sin(a)
        # The bracket: a plate shaped to the shell, wide at the top, its edges sunk just under the surface.
        N = 10
        verts, faces = [], []
        for i in range(N + 1):
            v = 2 * i / N - 1                            # -1 at the bottom of the plate, 1 at the top
            sv = 0.725 + 0.065 * v
            half = 0.014 + 0.014 * (v + 1) / 2
            for j in range(N + 1):
                u = 2 * j / N - 1
                ang = a + u * half / outer(sv)
                rr = outer(sv) - 0.0015 + 0.0075 * (1 - u ** 4) * (1 - v ** 4)
                verts.append((rr * math.cos(ang), rr * math.sin(ang), height * sv))
        for i in range(N):
            for j in range(N):
                q = i * (N + 1) + j
                faces.append((q, q + 1, q + N + 2, q + N + 1))
        mesh_object('lug', verts, faces, name, 'conga_hw').parent = g
        # The lug body on the plate, and the hook: a straight rod that turns over the hoop.
        r_top, r_lug = top_r + WALL + 0.008, outer(0.76) + 0.0115
        cyl(0.0085, 0.03, (r_lug * c, r_lug * s, height * 0.76), m='conga_hw', parent=g, cname=name, verts=24)
        limb((r_top * c, r_top * s, height - 0.006), (r_lug * c, r_lug * s, height * 0.76 + 0.012), 0.0035, 'chrome', g, name)
        r_over = top_r + 0.011
        limb((r_top * c, r_top * s, height - 0.006), (r_over * c, r_over * s, height + 0.008), 0.0035, 'chrome', g, name)
    for k in range(3):
        a = R(90 + 120 * k)
        blob((0.03, 0.03, 0.018), (foot_r * math.cos(a), foot_r * math.sin(a), 0.008), m='rubber', parent=g, cname=name)
    return g
conga('conga-small', (0.0, -0.9, 0.5), (R(10), 0, R(-20)), 0.62, 0.11, 0.125, 0.075)
conga('conga-large', (0.0, -0.9, 0.42), (R(10), 0, R(-20)), 0.78, 0.14, 0.16, 0.095)

EXTRAS = ('cowbell', 'chimes', 'gong', 'tambourine', 'maraca', 'shaker', 'claves', 'belltree', 'bucket', 'stack', 'snap', 'clap', 'stomp', 'stickclick', 'cajon', 'conga-small', 'conga-large')
FULL_MESH = ('snap', 'clap')    # their pictures use the whole hand-and-arm mesh; the hand-only one is just for the bare render
if MODE != 'pieces':
    for name in EXTRAS:
        for o in COLL[name].all_objects:
            o.hide_render = True
    for o in stack_objects + list(COLL['clap_arms'].all_objects) + list(COLL['snap_arms'].all_objects):
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

def render(path, objs=None):
    # border=auto renders only the part of the frame that objs cover (with a
    # margin, so the denoiser sees past the edges); the rest stays
    # transparent. The frame and the pixels inside are as in a full render, so
    # one piece's layer can be redone quickly and cropped as usual.
    scn.render.use_border = False
    if args.get('border') == 'auto' and objs:
        from bpy_extras.object_utils import world_to_camera_view
        dg = bpy.context.evaluated_depsgraph_get()
        xs, ys = [], []
        for o in objs:
            if o.type != 'MESH' or o.hide_render:
                continue
            oe = o.evaluated_get(dg)
            for c in oe.bound_box:
                v = world_to_camera_view(scn, cam, oe.matrix_world @ Vector(c))
                xs.append(v.x); ys.append(v.y)
        pad = 0.03
        scn.render.use_border = True; scn.render.use_crop_to_border = False
        scn.render.border_min_x, scn.render.border_max_x = max(0.0, min(xs) - pad), min(1.0, max(xs) + pad)
        scn.render.border_min_y, scn.render.border_max_y = max(0.0, min(ys) - pad), min(1.0, max(ys) + pad)
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)

if MODE == 'preview':
    render(os.path.join(OUT, 'preview.png'))
elif MODE == 'pieces':
    # Each piece on its own, nothing in front of it and no floor: the
    # pictures on the Kit tab's slot cards. Same camera and light as the
    # stage, so they look like the kit. (The stage layers can't be used for
    # this: each has holes where the pieces in front of it are.)
    pieces = ['kick', 'tom1', 'tom2', 'floor', 'snare', 'hihat', 'crash', 'crash2', 'splash', 'ride', 'china', *EXTRAS]
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
        if L in ('crash', 'crash2', 'ride', 'china', 'splash', 'stack', 'clap', 'snap', 'snare'):
            show_only(set(COLL[L].all_objects))
            render(os.path.join(OUT, 'pieces', L + '-bare.png'), set(COLL[L].all_objects))
        shown = set(HW[L]) if L in FULL_MESH else set(COLL[L].all_objects) | set(HW.get(L, []))
        show_only(shown)
        render(os.path.join(OUT, 'pieces', L + '.png'), shown)
        print('PIECE', L, flush=True)
else:
    layers = ['base', 'kick', 'tom1', 'tom2', 'floor', 'snare', 'hihat', 'crash', 'crash2', 'splash', 'ride', 'china']
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
        render(os.path.join(OUT, 'layers', L + '.png'), None if L == 'base' else mine)
        print('LAYER', L, flush=True)

if MODE == 'final':
    render(os.path.join(OUT, 'full.png'))
    layers = ['base', 'kick', 'tom1', 'tom2', 'floor', 'snare', 'hihat', 'crash', 'crash2', 'splash', 'ride', 'china']
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
