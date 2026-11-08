"""Create an original xianxia costume on the unchanged CC0 Human rig.
Run with Blender 4.5+: blender -b --python tools/art/generate_hero.py.
The studio preview is look development, not an in-game screenshot.
"""
from pathlib import Path
import argparse
import math
import json
import os
import sys
import hashlib
import bpy
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

ROOT = Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-blend",type=Path)
args=parser.parse_args(sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else [])
raw_root=Path(os.environ.get("QINGLAN_ASSETS_RAW",ROOT/".local/assets-raw"))
SOURCE = args.source_blend or raw_root / "quaternius/extracted/Animated Human by @Quaternius/Blend/Animated Human.blend"
OUT = ROOT / "assets/art-source"
EVIDENCE = ROOT / "docs/dev/evidence/visual-upgrade"
OUT.mkdir(parents=True, exist_ok=True)
EVIDENCE.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.open_mainfile(filepath=str(SOURCE))
rig = bpy.data.objects["Human Armature"]
body = bpy.data.objects["Human_Mesh"]
source_idle = bpy.data.actions.get("Idle")
if source_idle is None or rig.animation_data is None or rig.animation_data.action != source_idle:
    raise RuntimeError("The source Human rig must retain its original Idle action")
source_idle_slot = rig.animation_data.action_slot
bpy.context.scene.frame_set(77)
bpy.context.view_layer.update()
source_idle_pose = {bone.name: bone.matrix.copy() for bone in rig.pose.bones}
source_bind_pose = {bone.name: bone.matrix_local.copy() for bone in rig.data.bones}
rig.data.pose_position = "REST"
rig.animation_data_clear()
parts = []

def material(name, color, rough=.7, metal=0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes.get("Principled BSDF")
    p.inputs["Base Color"].default_value = (*color, 1)
    p.inputs["Roughness"].default_value = rough
    p.inputs["Metallic"].default_value = metal
    m.diffuse_color = (*color, 1)
    if "Cloth" in name:
        p.inputs["Sheen Weight"].default_value = .25
        noise = m.node_tree.nodes.new("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = 180
        noise.inputs["Detail"].default_value = 2
        bump = m.node_tree.nodes.new("ShaderNodeBump")
        bump.inputs["Strength"].default_value = .12
        bump.inputs["Distance"].default_value = .025
        m.node_tree.links.new(noise.outputs["Fac"], bump.inputs["Height"])
        m.node_tree.links.new(bump.outputs["Normal"], p.inputs["Normal"])
    return m

skin = material("Cultivator_Skin", (.52, .31, .20), .48)
cloth = material("Cultivator_JadeCloth", (.025, .105, .115), .72)
inner = material("Cultivator_IvoryCloth", (.58, .57, .46), .83)
edge = material("Cultivator_InkCloth", (.009, .022, .027), .77)
gold = material("Cultivator_AntiqueGold", (.45, .27, .075), .34, .78)
leather = material("Cultivator_Leather", (.025, .019, .014), .58)
hair = material("Cultivator_Hair", (.011, .009, .013), .66)
jade = material("Cultivator_Jade", (.03, .32, .25), .25, .12)
# Keep source palette samples at their existing UVs. Original accent swatches
# occupy four unused columns so modeled lips/eyes/nails need no extra slot.
img = bpy.data.images.load(str(SOURCE.parent / "Textures/NakedLightSkin.png"))
(OUT/"Textures").mkdir(exist_ok=True)
palette=np.array(img.pixels[:],dtype=np.float32).reshape((32,32,4))
source_palette=palette.copy()
palette[0:5,0:4]=(.80,.55,.44,1)
palette[5:10,0:4]=(.92,.89,.84,1)
palette[10:15,0:4]=(.16,.10,.065,1)
palette[15:20,0:4]=(.89,.64,.49,1)
palette[20:25,0:4]=(.94,.76,.64,1)
assert np.array_equal(source_palette[:,14:20],palette[:,14:20]), "Original Human UV samples must remain unchanged"
img.pixels.foreach_set(palette.ravel())
img.filepath_raw=str(OUT/"Textures/NakedLightSkin.png")
img.file_format="PNG"
img.save()
img.pack()
# A small project-original weave normal tiles on the garment UVs at runtime.
size=256
yy,xx=np.mgrid[0:size,0:size]
dx=.09*np.sin(xx*math.pi/4)*(.65+.35*np.cos(yy*math.pi/4))
dy=.09*np.sin(yy*math.pi/4)*(.65+.35*np.cos(xx*math.pi/4))
zz=np.ones_like(dx)
length=np.sqrt(dx*dx+dy*dy+zz*zz)
rgba=np.stack((.5+.5*dx/length,.5+.5*dy/length,.5+.5*zz/length,np.ones_like(dx)),axis=-1).astype(np.float32)
weave=bpy.data.images.new("CultivatorWeaveNormal",width=size,height=size,alpha=True)
weave.colorspace_settings.name="Non-Color"
weave.pixels.foreach_set(rgba.ravel())
weave.filepath_raw=str(OUT/"Textures/CultivatorWeaveNormal.png")
weave.file_format="PNG"
weave.save()
tex = skin.node_tree.nodes.new("ShaderNodeTexImage")
tex.image = img
skin.node_tree.links.new(tex.outputs["Color"], skin.node_tree.nodes.get("Principled BSDF").inputs["Base Color"])
body.data.materials.clear()
body.data.materials.append(skin)
body.data.materials.append(edge)
body.data.materials.append(hair)
for polygon in body.data.polygons:
    polygon.use_smooth = True
    # The original clothing/legs form a dark fitted underlayer. This avoids
    # exposed skin at a moving garment seam and retains hand/face UV details.
    hand_indices={g.index for g in body.vertex_groups if "Hand" in g.name}
    is_hand=any(any(g.group in hand_indices and g.weight>.12 for g in body.data.vertices[i].groups) for i in polygon.vertices)
    center_height=sum(body.data.vertices[i].co.y for i in polygon.vertices)/len(polygon.vertices)
    if center_height<6.53 and not is_hand:polygon.material_index=1
    uv=body.data.uv_layers.active.data
    if center_height>6.70:
        polygon.material_index=0
        for i in polygon.loop_indices:uv[i].uv=(.5,.90)
# Replace the source's fused hand shapes with separate skinned digits. The
# available Index chain drives four fingers together; the thumb keeps its rig.
bm=bmesh.new();bm.from_mesh(body.data)
replace_faces=[]
for f in bm.faces:
    side=sum(v.co.z for v in f.verts)/len(f.verts)
    if abs(side)>3.42:
        replace_faces.append(f)
bmesh.ops.delete(bm,geom=replace_faces,context="FACES")
loose=[v for v in bm.verts if not v.link_faces]
if loose:bmesh.ops.delete(bm,geom=loose,context="VERTS")
bm.to_mesh(body.data);bm.free()
bpy.context.view_layer.objects.active = body
body.select_set(True)
sub = body.modifiers.new("Silhouette refinement", "SUBSURF")
sub.levels = 1
sub.render_levels = 1
bpy.ops.object.modifier_apply(modifier=sub.name)
# Small cheek, chin and jaw changes break the flat lower-face planes.
for v in body.data.vertices:
    h=v.co.y;s=abs(v.co.z)
    if h>7.46:
        # Seat the former coarse hair silhouette inside the new scalp shell.
        x=(v.co.x+.035)/.64;y=(h-7.45)/.615;z=v.co.z/.535
        radius=math.sqrt(x*x+y*y+z*z)
        if radius>1:
            v.co.x=-.035+(v.co.x+.035)/radius
            v.co.y=7.45+(h-7.45)/radius
            v.co.z/=radius
    if 6.7<h<7.38 and v.co.x>.15:
        cheek=.035*math.exp(-((h-7.20)/.17)**2-((s-.28)/.16)**2)
        chin=.020*math.exp(-((h-6.83)/.10)**2-(s/.18)**2)
        v.co.x+=cheek+chin
        v.co.z*=1-.028*math.exp(-((h-6.85)/.12)**2)
    if 7.18<h<7.56 and v.co.x>.20:
        blend=.90*math.exp(-((h-7.38)/.14)**2-((s-.235)/.13)**2)
        socket=.39-.10*(s/.5)**2
        v.co.x=v.co.x*(1-blend)+socket*blend
body.name = "Cultivator_Body"
parts.append(body)

# Costume coordinates: (side, forward, height); source faces positive X.
def source_point(p):
    return (p[1], p[2], -p[0])

def canonical(p):
    return Vector((-p.z, p.x, p.y))

def mesh(name, vertices, faces, mat, weights):
    data = bpy.data.meshes.new(name)
    # The source-coordinate permutation mirrors handedness. Reverse winding
    # so opaque Unreal materials render the exterior rather than the inside.
    data.from_pydata([source_point(p) for p in vertices], [], [tuple(reversed(f)) for f in faces])
    data.update()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.matrix_world = rig.matrix_world.copy()
    data.materials.append(mat)
    for p in data.polygons:
        p.use_smooth = True
    for i, p in enumerate(vertices):
        groups = weights(p) if callable(weights) else {weights: 1.0}
        for bone, weight in groups.items():
            if weight <= 0:
                continue
            group = obj.vertex_groups.get(bone) or obj.vertex_groups.new(name=bone)
            group.add([i], weight, "REPLACE")
    mod = obj.modifiers.new("Human rig", "ARMATURE")
    mod.object = rig
    obj.parent = rig
    obj.matrix_parent_inverse = rig.matrix_world.inverted()
    obj.matrix_world = rig.matrix_world.copy()
    uv = data.uv_layers.new(name="UVMap")
    for poly in data.polygons:
        for li in poly.loop_indices:
            p = vertices[data.loops[li].vertex_index]
            uv.data[li].uv = (math.atan2(p[1], p[0]) / (2*math.pi) + .5, p[2] / 8.4)
    parts.append(obj)
    return obj

def skin_uv(obj, accent="skin"):
    colors={"skin":(.5,.90),"lip":(.0625,.08),"sclera":(.0625,.22),
            "iris":(.0625,.39),"pupil":(.5,.60),"cheek":(.0625,.55),"nail":(.0625,.70)}
    for uv in obj.data.uv_layers.active.data:uv.uv=colors[accent]
    return obj

def torso_weights(p):
    h = p[2]
    stops = [(4.2,"Hips"), (4.8,"Spine"), (5.45,"Spine1"), (6.0,"Spine2")]
    for (lo, a), (hi,b) in zip(stops,stops[1:]):
        if lo <= h <= hi:
            t=(h-lo)/(hi-lo)
            return {a:1-t,b:t}
    return {"Hips" if h<4.2 else "Spine2":1}

def rings(name, spec, mat, weights, segments=64):
    verts=[]
    for height, width, depth in spec:
        for i in range(segments):
            a=2*math.pi*i/segments
            fold=1+.014*math.cos(12*a)
            verts.append((width*math.cos(a)*fold, depth*math.sin(a)*fold, height))
    faces=[]
    for j in range(len(spec)-1):
        for i in range(segments):
            k=j*segments+i; n=j*segments+(i+1)%segments
            faces.append((k,n,n+segments,k+segments))
    return mesh(name,verts,faces,mat,weights)

def path(name, points, radius, mat, bone, sides=6):
    verts=[]
    for i,p in enumerate(points):
        p=Vector(p)
        tangent=(Vector(points[min(i+1,len(points)-1)])-Vector(points[max(i-1,0)])).normalized()
        cross=tangent.cross(Vector((0,0,1)))
        if cross.length<.01: cross=tangent.cross(Vector((0,1,0)))
        u=cross.normalized(); v=tangent.cross(u).normalized()
        r=radius(i/(len(points)-1)) if callable(radius) else radius
        for k in range(sides):
            a=k*2*math.pi/sides
            verts.append(p+r*(u*math.cos(a)+v*math.sin(a)))
    faces=[]
    for i in range(len(points)-1):
        for k in range(sides):
            q=i*sides+k; n=i*sides+(k+1)%sides
            faces.append((q,n,n+sides,q+sides))
    return mesh(name,verts,faces,mat,bone)

def ellipsoid(name, center, scale, mat, bone, seg=24, rows=14):
    verts=[]
    for j in range(rows+1):
        phi=math.pi*j/rows
        for i in range(seg):
            a=2*math.pi*i/seg
            unit=[math.sin(phi)*math.cos(a),math.sin(phi)*math.sin(a),math.cos(phi)]
            verts.append(tuple(center[k]+scale[k]*unit[k] for k in range(3)))
    faces=[]
    for j in range(rows):
        for i in range(seg):
            k=j*seg+i; n=j*seg+(i+1)%seg
            faces.append((k,n,n+seg,k+seg))
    return mesh(name,verts,[tuple(reversed(f)) for f in faces],mat,bone)

# Facial details follow the actual refined surface, not guessed floating planes.
face_bvh=BVHTree.FromPolygons([v.co[:] for v in body.data.vertices],
                             [tuple(p.vertices) for p in body.data.polygons])
def face_depth(side,height):
    location,normal,index,distance=face_bvh.ray_cast(Vector((2,height,-side)),Vector((-1,0,0)))
    return location.x if location else .40

# Lips have an upper bow, rounded lower lip and a restrained dark mouth crease.
mouth_height=6.96
for upper in (True,False):
    vs=[];fs=[];columns=28;bands=5
    for j in range(bands):
        t=j/(bands-1)
        for i in range(columns):
            x=(i/(columns-1)-.5)*.30
            taper=max(0,1-(x/.15)**2)
            bow=.015*math.exp(-((abs(x)-.065)/.05)**2)
            h=mouth_height+(1 if upper else -1)*t*(.024*taper+bow*(1 if upper else .2))
            d=face_depth(x,h)+.004+.016*math.sin(math.pi*t)*taper
            vs.append((x,d,h))
    for j in range(bands-1):
        for i in range(columns-1):
            k=j*columns+i;fs.append((k,k+1,k+1+columns,k+columns))
    if upper:fs=[tuple(reversed(f)) for f in fs]
    skin_uv(mesh("Sculpted upper lip" if upper else "Sculpted lower lip",vs,fs,skin,"Head"),"lip")
pts=[]
for i in range(29):
    x=(i/28-.5)*.295
    h=mouth_height+.003*math.cos(x/.15*math.pi)
    pts.append((x,face_depth(x,h)+.009,h))
skin_uv(path("Mouth crease",pts,.0055,skin,"Head",6),"iris")
for sign in (-1,1):
    x=sign*.062;h=7.067
    d=face_depth(x,h)
    skin_uv(ellipsoid("Nose wing",(x,d+.006,h),(.044,.026,.030),skin,"Head",16,10))
    skin_uv(ellipsoid("Nostril shadow",(sign*.062,d+.020,h-.017),(.018,.009,.011),skin,"Head",12,6),"iris")
    x=sign*.235;h=7.382
    d=face_depth(x,h)
    skin_uv(ellipsoid("Eye sclera",(x,d+.006,h),(.095,.024,.032),skin,"Head",16,10),"sclera")
    skin_uv(ellipsoid("Eye iris",(x,d+.030,h-.002),(.025,.008,.026),skin,"Head",16,8),"iris")
    skin_uv(ellipsoid("Eye pupil",(x,d+.036,h-.002),(.011,.006,.020),skin,"Head",12,6),"pupil")
    vs=[];fs=[];segments=36;bands=4
    for j in range(bands):
        t=j/(bands-1)
        for i in range(segments):
            a=i*2*math.pi/segments
            width=.118*(1-t)+.088*t
            height=(.050*(1-t)+.028*t) if math.sin(a)>=0 else (.044*(1-t)+.019*t)
            xx=x+width*math.cos(a);hh=h+height*math.sin(a)
            vs.append((xx,face_depth(xx,hh)+.001+.017*t,hh))
    for j in range(bands-1):
        for i in range(segments):
            k=j*segments+i;n=j*segments+(i+1)%segments
            fs.append((k,n,n+segments,k+segments))
    skin_uv(mesh("Sculpted eyelid transition",vs,[tuple(reversed(f)) for f in fs],skin,"Head"))
    brow=[]
    for i in range(17):
        t=i/16;xx=x+(t-.5)*.20;hh=h+.12+.018*math.sin(t*math.pi)
        brow.append((xx,face_depth(xx,hh)+.012,hh))
    path("Refined eyebrow",brow,.010,hair,"Head",6)

# Five distinct digits retain the existing Hand/Thumb/Index bone hierarchy.
for side in ("Left","Right"):
    sign=1 if side=="Left" else -1
    wrist=canonical(rig.data.bones[side+"Hand"].head_local)
    index_points=[canonical(rig.data.bones[side+"HandIndex"+str(i)].head_local) for i in range(1,5)]
    index_points.append(canonical(rig.data.bones[side+"HandIndex4"].tail_local))
    palm=(wrist+index_points[0])/2
    skin_uv(ellipsoid(side+" articulated palm",palm,(.24,.22,.11),skin,side+"Hand",20,12))
    for finger,offset,length in [("index",.09,1.0),("middle",-.025,1.08),("ring",-.14,.98),("little",-.245,.80)]:
        start=index_points[0]+Vector((0,offset,0))
        points=[start+(p-index_points[0])*length for p in index_points]
        for i in range(len(points)):points[i].z+=.009*math.sin(i*math.pi/4)
        def finger_weights(p,points=points,side=side):
            distances=[(Vector(p)-(points[i]+points[i+1])/2).length for i in range(4)]
            n=min(range(4),key=lambda i:distances[i])
            return {side+"HandIndex"+str(n+1):1}
        radius=.055 if finger!="little" else .046
        skin_uv(path(side+" "+finger+" finger",points,radius,skin,finger_weights,8))
        skin_uv(ellipsoid(side+" "+finger+" fingertip",points[-1],(radius,radius,radius),skin,side+"HandIndex4",12,6))
        skin_uv(ellipsoid(side+" "+finger+" knuckle",points[0],(.072,.060,.054),skin,side+"HandIndex1",12,6))
        nail=points[-1]+Vector((-sign*.026,0,.041))
        skin_uv(ellipsoid(side+" "+finger+" nail",nail,(.030,.034,.009),skin,side+"HandIndex4",10,4),"nail")
    thumb_points=[canonical(rig.data.bones[side+"HandThumb"+str(i)].head_local) for i in range(1,5)]
    thumb_points.append(canonical(rig.data.bones[side+"HandThumb4"].tail_local))
    def thumb_weights(p,points=thumb_points,side=side):
        n=min(range(4),key=lambda i:(Vector(p)-(points[i]+points[i+1])/2).length)
        return {side+"HandThumb"+str(n+1):1}
    skin_uv(path(side+" thumb",thumb_points,.065,skin,thumb_weights,8))
    skin_uv(ellipsoid(side+" thumb tip",thumb_points[-1],(.063,.063,.063),skin,side+"HandThumb4",12,6))

rings("Inner crossed tunic",[(4.0,.82,.68),(4.5,.81,.68),(5,.85,.70),(5.5,.89,.71),(6,.91,.71),(6.35,.94,.66),(6.56,.47,.39)],inner,torso_weights)
# Open-front robe: textile folds, bound edges, flexible hip/leg weights.
verts=[]; faces=[]; seg=72; rows=28
def coat_radius(t):
    h=1.2+t*5.12
    if h<4.2:
        blend=(h-1.2)/3
        return (1.16-.28*blend,.81-.06*blend)
    blend=(h-4.2)/2.12
    return (.88+.16*blend,.75+.04*blend)
for j in range(rows):
    t=j/(rows-1); h=1.2+t*5.12
    width,depth=coat_radius(t)
    for i in range(seg):
        a=math.pi/2+.70+(2*math.pi-1.40)*i/(seg-1)
        fold=.05*math.cos(16*a)*max(0,1-t)
        verts.append((math.cos(a)*(width+fold), math.sin(a)*(depth+fold)-.02,h))
# Shoulder fabric slopes to a narrow collar instead of leaving an open cylinder.
for i in range(seg):
    a=math.pi/2+.70+(2*math.pi-1.40)*i/(seg-1)
    verts.append((math.cos(a)*.53, math.sin(a)*.46,6.61))
rows+=1
for j in range(rows-1):
    for i in range(seg-1):
        k=j*seg+i;faces.append((k,k+1,k+1+seg,k+seg))
def robe_weights(p):
    if p[2]>=4.2: return torso_weights(p)
    side="LeftUpLeg" if p[0]>0 else "RightUpLeg"
    t=max(0,min(.9,(4.2-p[2])/2.8))
    return {"Hips":1-t,side:t}
mesh("Jade outer robe",verts,faces,cloth,robe_weights)
for sign in (-1,1):
    points=[]
    for j in range(30):
        t=j/29; h=1.2+t*5.1; a=math.pi/2+.70 if sign==1 else math.pi/2+2*math.pi-.70
        width,depth=coat_radius(t)
        points.append((math.cos(a)*width,math.sin(a)*depth-.025,h))
    path("Robe bound edge",points,.065,edge,robe_weights)
    collar=[(sign*.10,.73,4.55),(sign*.23,.74,4.9),(sign*.45,.76,5.35),(sign*.65,.75,5.85),(sign*.38,.62,6.35)]
    path("Cross collar",collar,.055,edge,torso_weights,12)
    path("Collar gold piping",[(p[0]+sign*.055,p[1]+.012,p[2]) for p in collar],.012,gold,torso_weights)
rings("Woven waist sash",[(4.08,.94,.80),(4.16,.95,.81),(4.37,.94,.81),(4.44,.94,.80)],edge,"Hips")
for h in (4.1,4.42):
    pts=[(.955*math.cos(i*2*math.pi/96),.825*math.sin(i*2*math.pi/96),h) for i in range(97)]
    path("Sash piping",pts,.022,gold,"Hips")
ellipsoid("Jade belt seal",(0,.855,4.26),(.15,.045,.18),jade,"Hips")
for sign in (-1,1):
    path("Pendant chain",[(sign*.27,.85,4.26),(sign*.31,.85,3.9),(sign*.33,.85,3.55)],.017,gold,"Hips")
    ellipsoid("Pendant",(sign*.33,.85,3.51),(.075,.032,.11),jade,"Hips",20,12)
    for k in range(7):
        path("Silk tassel",[(sign*.33+(k-3)*.017,.85,3.43),(sign*.33+(k-3)*.022,.85,3.12)],.01,gold,"Hips",5)
for side in ("Left","Right"):
    shoulder=canonical(rig.data.bones[side+"Arm"].head_local)
    elbow=canonical(rig.data.bones[side+"ForeArm"].head_local)
    wrist=canonical(rig.data.bones[side+"Hand"].head_local)
    for label,start,end,radii,mat,bone in [("Sleeve",shoulder,elbow,(.47,.38),cloth,side+"Arm"),("Forearm wrap",elbow,wrist,(.29,.22),edge,side+"ForeArm")]:
        axis=(end-start).normalized();u=axis.cross(Vector((0,0,1))).normalized();v=axis.cross(u).normalized()
        vs=[];fs=[];segs=40;steps=12
        for j in range(steps):
            t=j/(steps-1);r=radii[0]*(1-t)+radii[1]*t
            for i in range(segs):
                a=i*2*math.pi/segs
                p=start.lerp(end,t)+(u*math.cos(a)+v*math.sin(a))*r
                if label=="Sleeve":p.z-=.12*math.sin(math.pi*t)*(1+math.sin(a))
                vs.append(p)
        for j in range(steps-1):
            for i in range(segs):
                k=j*segs+i;n=j*segs+(i+1)%segs;fs.append((k,n,n+segs,k+segs))
        mesh(side+label,vs,fs,mat,bone)
        for t in ([.1,.9] if label=="Sleeve" else [.18,.48,.78]):
            center=start.lerp(end,t);r=radii[0]*(1-t)+radii[1]*t+.02
            pts=[center+(u*math.cos(i*2*math.pi/48)+v*math.sin(i*2*math.pi/48))*r for i in range(49)]
            path(side+" cuff binding",pts,.025,gold,bone)
    sign=1 if side=="Left" else -1
    vs=[];fs=[]
    for j in range(10):
        t=j/9;h=.2+t*1.35;cx=sign*(.50-.04*t)
        for i in range(40):
            a=i*2*math.pi/40;vs.append((cx+.29*math.cos(a),-.10+.31*math.sin(a),h))
    for j in range(9):
        for i in range(40):
            k=j*40+i;n=j*40+(i+1)%40;fs.append((k,n,n+40,k+40))
    mesh(side+" boot shaft",vs,fs,leather,side+"Leg")
    ellipsoid(side+" boot toe",(sign*.51,.27,.17),(.34,.70,.28),leather,side+"Foot")
    for h in (.55,1.40):
        pts=[(sign*.50+.305*math.cos(i*2*math.pi/48),-.1+.325*math.sin(i*2*math.pi/48),h) for i in range(49)]
        path(side+" boot seam",pts,.022,gold,side+"Leg")
# Scalloped scalp surface and swept ridges give hair a lock structure rather
# than one glossy helmet. All geometry still follows the original Head bone.
vs=[];fs=[];segs=56;rows=16
def scalp(a,t):
    front=(1+math.sin(a))/2
    limit=2.25-1.35*front+.35*front*abs(math.cos(a))+.08*math.sin(2*a)
    phi=t*limit
    ripple=1+.020*math.cos(32*a+phi*2.0)
    return (.57*math.sin(phi)*math.cos(a)*ripple,
            -.035+.69*math.sin(phi)*math.sin(a)*ripple,
            7.45+.65*math.cos(phi))
for j in range(rows):
    for i in range(segs):vs.append(scalp(i*2*math.pi/segs,j/(rows-1)))
for j in range(rows-1):
    for i in range(segs):
        k=j*segs+i;n=j*segs+(i+1)%segs;fs.append((k,n,n+segs,k+segs))
mesh("Sculpted swept hair cap",vs,[tuple(reversed(f)) for f in fs],hair,"Head")
for i in range(24):
    a=i*2*math.pi/24
    pts=[]
    for j in range(16):
        t=.10+.89*j/15
        p=Vector(scalp(a+.11*math.sin(t*math.pi),t))
        p.x*=1.013;p.y*=1.013
        pts.append(p)
    path("Swept scalp strand",pts,.009,hair,"Head",5)
ellipsoid("Bound topknot",(0,-.04,8.03),(.27,.29,.32),hair,"Head")
for i in range(16):
    a=i*2*math.pi/16
    pts=[]
    for j in range(14):
        phi=.14+2.9*j/13
        pts.append((.275*math.sin(phi)*math.cos(a),-.04+.295*math.sin(phi)*math.sin(a),8.03+.325*math.cos(phi)))
    path("Topknot strand",pts,.009,hair,"Head",5)
pts=[(.28*math.cos(i*2*math.pi/48),-.04+.30*math.sin(i*2*math.pi/48),8.03) for i in range(49)]
path("Hair binding",pts,.031,gold,"Head")
path("Hair pin",[(-.40,-.04,8.08),(.40,-.04,8.08)],.035,gold,"Head",12)
for i in range(9):
    x=(i/8-.5)*.68
    pts=[]
    for j in range(24):
        t=j/23
        pts.append((x*(1-.22*t)+.03*math.sin(t*math.pi+i),
                    -.59-.10*math.sin(t*math.pi),7.73-t*(1.72+.12*math.cos(i))))
    def hair_weights(p):
        t=max(0,min(1,(p[2]-6.0)/1.4))
        return {"Spine2":1-t,"Head":t}
    path("Tapered long hair lock",pts,lambda t:.041*(.25+.75*math.sin(math.pi*(.10+.86*t))),hair,hair_weights,10)
for sign in (-1,1):
    for baseh in (5.0,5.48):
        pts=[]
        for i in range(25):
            a=i*1.7*math.pi/24
            pts.append((sign*(.40+.11*math.cos(a)),.755,baseh+.08*math.sin(a)))
        path("Cloud embroidery",pts,.012,gold,torso_weights,6)
bpy.ops.object.select_all(action="DESELECT")
for p in parts:p.select_set(True)
bpy.context.view_layer.objects.active=body
bpy.ops.object.join()
hero=body;hero.name="SK_Cultivator"
# Keep the original eight-slot import contract despite additional skin/hair parts.
ordered=[skin,edge,inner,cloth,gold,jade,leather,hair]
indices={i:ordered.index(m) for i,m in enumerate(hero.data.materials)}
polygon_slots=[indices[p.material_index] for p in hero.data.polygons]
hero.data.materials.clear()
for m in ordered:hero.data.materials.append(m)
for p,slot in zip(hero.data.polygons,polygon_slots):p.material_index=slot
for mod in list(hero.modifiers):
    if mod.type=="ARMATURE":hero.modifiers.remove(mod)
mod=hero.modifiers.new("Human rig","ARMATURE");mod.object=rig
bpy.ops.object.select_all(action="DESELECT")
hero.select_set(True);rig.select_set(True)
bpy.context.view_layer.objects.active=hero
# Match the original FBX's default Idle pose while keeping the unchanged rest
# bind matrices. Blender exports mesh vertices/bind matrices in rest pose, and
# Model transforms from the current pose; UE's T0 import maps between them.
rig.data.pose_position = "POSE"
rig.animation_data_create()
rig.animation_data.action = source_idle
if source_idle_slot is not None:
    rig.animation_data.action_slot = source_idle_slot
bpy.context.scene.frame_set(77)
bpy.context.view_layer.update()
def matrix_error(a, b):
    return max(abs(a[row][column]-b[row][column]) for row in range(4) for column in range(4))
default_pose_error = max(matrix_error(bone.matrix, source_idle_pose[bone.name]) for bone in rig.pose.bones)
bind_pose_error = max(matrix_error(bone.matrix_local, source_bind_pose[bone.name]) for bone in rig.data.bones)
if default_pose_error > 1e-6 or bind_pose_error > 1e-6:
    raise RuntimeError(f"Human rig changed: default pose {default_pose_error}, bind pose {bind_pose_error}")
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/"SK_Cultivator.blend"),compress=True)
bpy.ops.export_scene.fbx(filepath=str(OUT/"SK_Cultivator.fbx"),use_selection=True,object_types={"MESH","ARMATURE"},add_leaf_bones=True,bake_anim=False,apply_unit_scale=True,axis_forward="-Z",axis_up="Y",mesh_smooth_type="FACE",path_mode="COPY",embed_textures=True)
report={"source":"CC0 Quaternius Animated Human; original costume by Qinglan","vertices":len(hero.data.vertices),"triangles":sum(len(p.vertices)-2 for p in hero.data.polygons),"bones":[b.name for b in rig.data.bones],"materials":[m.name for m in hero.data.materials],"preview":"Blender studio look development; not gameplay","rig_scale":list(rig.scale)}
report["source_palette_columns_14_to_19_preserved"]=True
report["source_palette_sha256"]=hashlib.sha256((SOURCE.parent/"Textures/NakedLightSkin.png").read_bytes()).hexdigest()
report["source_blend_sha256"]=hashlib.sha256(SOURCE.read_bytes()).hexdigest()
report["export_reference_pose"]={"action":source_idle.name,"frame":77,"bake_anim":False,"max_default_pose_matrix_error":default_pose_error,"max_rest_bind_matrix_error":bind_pose_error}
report["face_pass"]="Modeled lips/crease, nostrils, sclera/iris/pupils/eyelids, cheek/chin/jaw transitions, separate digits and matte swept hair locks"
report["material_parameters"]=[{"slot":i,"name":m.name,"base_color_linear":list(m.diffuse_color),"roughness":m.node_tree.nodes.get("Principled BSDF").inputs["Roughness"].default_value,"metallic":m.node_tree.nodes.get("Principled BSDF").inputs["Metallic"].default_value,"base_color_texture":"assets/art-source/Textures/NakedLightSkin.png" if m==skin else None} for i,m in enumerate(hero.data.materials)]
(EVIDENCE/"hero-model.json").write_text(json.dumps(report,indent=2)+"\n")
print("HERO_ASSET_REPORT",json.dumps(report))
rig.data.pose_position="POSE"
idle=bpy.data.actions.get("Idle")
if idle:
    rig.animation_data_create();rig.animation_data.action=idle
    bpy.context.scene.frame_set(1)
scene=bpy.context.scene
scene.render.engine="CYCLES"
scene.cycles.samples=48;scene.cycles.use_denoising=True
scene.render.resolution_x=1000;scene.render.resolution_y=1200;scene.render.resolution_percentage=100
scene.view_settings.view_transform="AgX"
scene.world=bpy.data.worlds.new("Studio world");scene.world.use_nodes=True
scene.world.node_tree.nodes.get("Background").inputs["Color"].default_value=(.04,.06,.07,1)
scene.world.node_tree.nodes.get("Background").inputs["Strength"].default_value=.35
bpy.ops.mesh.primitive_plane_add(size=200,location=(0,0,-.06))
floor=bpy.context.object;floor.name="Studio floor";floor.data.materials.append(material("Studio slate",(.025,.045,.052),.8))
def aim(obj,target):obj.rotation_euler=(Vector(target)-obj.location).to_track_quat("-Z","Y").to_euler()
def light(name,loc,power,size,color):
    data=bpy.data.lights.new(name,"AREA");data.energy=power;data.shape="DISK";data.size=size;data.color=color
    obj=bpy.data.objects.new(name,data);bpy.context.collection.objects.link(obj);obj.location=loc;aim(obj,(0,0,2.7))
light("Warm key",(7,-5,8),1250,5,(1,.85,.68))
light("Cool fill",(4,5,4.5),850,4,(.65,.83,1))
light("Rim",(-4,1,7),1700,4,(.55,.88,1))
data=bpy.data.cameras.new("Hero camera");camera=bpy.data.objects.new("Hero camera",data);bpy.context.collection.objects.link(camera)
camera.location=(5,-13,5.2);aim(camera,(0,0,2.85));data.type="ORTHO";data.ortho_scale=6.7;scene.camera=camera
scene.render.filepath=str(EVIDENCE/"hero-preview.png")
bpy.ops.render.render(write_still=True)
head_world=rig.matrix_world@rig.pose.bones["Head"].head
camera.location=head_world+Vector((2.5,-7,1.2))
aim(camera,head_world+Vector((0,0,.45)))
data.ortho_scale=1.6
scene.render.resolution_x=800;scene.render.resolution_y=800
scene.render.filepath=str(EVIDENCE/"hero-face-preview.png")
bpy.ops.render.render(write_still=True)
report["sha256"]={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
                  [OUT/"SK_Cultivator.fbx",OUT/"SK_Cultivator.blend",OUT/"Textures/NakedLightSkin.png",
                   OUT/"Textures/CultivatorWeaveNormal.png",EVIDENCE/"hero-preview.png",EVIDENCE/"hero-face-preview.png"]}
(EVIDENCE/"hero-model.json").write_text(json.dumps(report,indent=2)+"\n")
