#!/usr/bin/env python3
"""check_anim_export.py — prove an animated .glb IS the pose the viewport drew.

    python3 tools/check_anim_export.py <animated.glb> <posed.glb> <frame>

Export the same model twice from the same clip and frame — once with
--export (a baked pose, vertices pre-skinned) and once with --exportanim or
--exportsceneanim (rigged, with curves) — then run this. It evaluates the
animated file's node transforms at frame/30 s, skins its vertices with its own
inverse bind matrices, and reports the largest disagreement with the baked
file, vertex for vertex, across every mesh and every skin.

Anything above about 1e-05 is a real difference; float32 agreement lands near
3e-07. This is the bar for any geometric change in this project — the
animation exporter (8c/8e) was accepted on 3.2e-07 over 2,527 vertices and
3.8e-07 over 7,887 vertices in a three-skin composed scene, and NOT on how the
result looked.

Needs numpy and nothing else. Both files must come from the same model with
the same hidden groups, or the mesh counts will not line up.
"""
import json,struct,sys,numpy as np
def load(p):
    d=open(p,'rb').read(); assert d[:4]==b'glTF'
    off=12; js=None; b=None
    while off<len(d):
        ln,ty=struct.unpack_from('<II',d,off); off+=8
        if ty==0x4E4F534A: js=json.loads(d[off:off+ln].decode())
        elif ty==0x004E4942: b=d[off:off+ln]
        off+=ln
    return js,b
CT={5126:('f4',4),5123:('u2',2),5121:('u1',1),5125:('u4',4)}
NC={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}
def acc(g,b,i):
    a=g['accessors'][i]; bv=g['bufferViews'][a['bufferView']]
    dt,sz=CT[a['componentType']]; n=NC[a['type']]
    o=bv.get('byteOffset',0)+a.get('byteOffset',0)
    st=bv.get('byteStride') or n*sz
    if st==n*sz:
        arr=np.frombuffer(b,dtype=np.dtype('<'+dt),count=a['count']*n,offset=o)
        return arr.reshape(a['count'],n)
    out=np.zeros((a['count'],n),dtype=np.dtype('<'+dt))
    for k in range(a['count']):
        out[k]=np.frombuffer(b,dtype=np.dtype('<'+dt),count=n,offset=o+k*st)
    return out
def qm(q):
    x,y,z,w=q
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]],dtype=np.float64)
def trs(t,q):
    M=np.eye(4); M[:3,:3]=qm(q); M[:3,3]=t; return M

ga,ba=load(sys.argv[1]); gp,bp=load(sys.argv[2]); FRAME=int(sys.argv[3]); FPS=30.0
nodes=ga['nodes']
local=[trs(np.array(n.get('translation',[0,0,0]),float),
           np.array(n.get('rotation',[0,0,0,1]),float)) for n in nodes]
an=ga['animations'][0]; t_target=FRAME/FPS
for ch in an['channels']:
    s=an['samplers'][ch['sampler']]
    times=np.asarray(acc(ga,ba,s['input']),float).ravel()
    out=np.asarray(acc(ga,ba,s['output']),float)
    i=int(np.argmin(np.abs(times-t_target)))
    assert abs(times[i]-t_target)<1e-6
    nd=ch['target']['node']; path=ch['target']['path']; v=out[i]
    if path=='rotation':
        local[nd][:3,:3]=qm(v)
    elif path=='translation':
        local[nd][:3,3]=v
parent={}
for i,n in enumerate(nodes):
    for c in n.get('children',[]): parent[c]=i
glob=[None]*len(nodes)
def world(i):
    if glob[i] is None:
        p=parent.get(i)
        glob[i]= local[i] if p is None else world(p)@local[i]
    return glob[i]
# mesh index -> (node, skin)
meshnode={}
for i,n in enumerate(nodes):
    if 'mesh' in n: meshnode[n['mesh']]=(i,n.get('skin'))
skincache={}
def skinmats(si):
    if si in skincache: return skincache[si]
    sk=ga['skins'][si]
    ibm=np.asarray(acc(ga,ba,sk['inverseBindMatrices']),float).reshape(-1,16)
    M=np.array([world(j)@ibm[k].reshape(4,4).T for k,j in enumerate(sk['joints'])])
    skincache[si]=M; return M
maxerr=0.0; tot=0
for mi in range(len(ga['meshes'])):
    prim=ga['meshes'][mi]['primitives'][0]
    P=np.asarray(acc(ga,ba,prim['attributes']['POSITION']),float)
    pp=gp['meshes'][mi]['primitives'][0]
    Pp=np.asarray(acc(gp,bp,pp['attributes']['POSITION']),float)
    if P.shape!=Pp.shape:
        print("mesh",mi,"shape mismatch",P.shape,Pp.shape); continue
    _,si=meshnode[mi]
    if si is None:
        maxerr=max(maxerr,np.abs(P-Pp).max()); tot+=len(P); continue
    J=np.asarray(acc(ga,ba,prim['attributes']['JOINTS_0']),np.int64)
    W=np.asarray(acc(ga,ba,prim['attributes']['WEIGHTS_0']),float)
    M=skinmats(si)
    h=np.concatenate([P,np.ones((len(P),1))],1)
    o=np.zeros((len(P),3))
    for k in range(J.shape[1]):
        o+=np.einsum('nij,nj->ni',M[J[:,k]],h)[:,:3]*W[:,k:k+1]
    maxerr=max(maxerr,np.abs(o-Pp).max()); tot+=len(P)
print("meshes:",len(ga['meshes']),"verts:",tot,"max |animated - posed| =",maxerr)
