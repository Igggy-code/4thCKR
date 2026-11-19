import os as _os
_TDIR=_os.path.normpath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))+'/'
"""Rebuild a Conker CAFF font with real Cyrillic glyphs (U+0401..U+0451).
Keeps ASCII + a few symbols from the original atlas, drops accented Latin
(their codes map to the base letter), renders Cyrillic from PT Sans Caption Bold
(OFL), repacks the 8-bit atlas and rewrites glyph records + the code hash table."""
import struct, sys, unicodedata, numpy as np
from PIL import Image, ImageDraw, ImageFont

def h(c): return (c + (c >> 5) + 2) & 2047

LEVELS=None
def build(src, dst, ttf, ttf_index, size, baseline=None, keep_extra=(), preview=None, stroke=0, drop=(), symbols=True, extra_glyphs=True, replace_all=False, hscale=1.0):
    b=bytearray(open(src,'rb').read())
    dsz=struct.unpack_from('<I',b,0x50)[0]; gsz=struct.unpack_from('<I',b,0x78)[0]
    D=0x180; G=len(b)-gsz
    d=b[D:D+dsz]
    cm_off=struct.unpack_from('<I',d,0x344)[0]; ng=struct.unpack_from('<I',d,0x354)[0]
    gl_off=struct.unpack_from('<I',d,0x370)[0]
    TW,TH=struct.unpack_from('<HH',d,0x380)
    tex=np.frombuffer(bytes(b[G:G+TW*TH]),np.uint8).reshape(TH,TW).copy()
    VS=256/TH; US=256/TW   # v coordinates are in a 256-high space
    levels=np.unique(tex)
    glyphs=[]
    for k in range(ng):
        o=gl_off+k*18
        xo,yo,w,hh=struct.unpack_from('<bbBB',d,o)
        u0,u1,v0,v1,adv,kern,lh=struct.unpack_from('<7H',d,o+4)
        glyphs.append(dict(xo=xo,yo=yo,w=w,h=hh,u0=u0,u1=u1,v0=v0,v1=v1,adv=adv,kern=kern,lh=lh))
    cmap={}
    for s in range(2048):
        c,g=struct.unpack_from('<HH',d,cm_off+4*s)
        if c!=0xffff: cmap[c]=g
    # baseline = yo+lh
    if baseline is None:
        g=glyphs[cmap[ord('H')]]; baseline=g['yo']+g['lh']
    def img_of(g):
        if g['w']==0 or g['h']==0: return None
        x=int(round(g['u0']/64/US-0.5)); y=int(round((g['v0']/64)/VS-0.5))
        return tex[y:y+g['h'],x:x+g['w']]
    SYM=(0xA1,0xA2,0xA3,0xA5,0xA7,0xA9,0xAB,0xAE,0xB0,0xB1,0xBB,0xBF,0xD7,0xF7,0x192,0x2018,0x2019,0x2026,0x20AC,0x25A1) if symbols else ()
    keep=[c for c in list(range(32,127))+list(SYM)+list(keep_extra) if c in cmap and c not in drop]
    items=[]   # (codes, record, image)
    if not replace_all:
      for c in keep:
        g=dict(glyphs[cmap[c]]); items.append(([c],g,img_of(g)))
    # Cyrillic
    if size<=0:   # auto: match ink height of 'H'
        ref=img_of(glyphs[cmap[ord('H')]]); rh=(ref.max(1)>60).sum()
        for size in range(10,80):
            f=ImageFont.truetype(ttf,size,index=ttf_index); im=Image.new('L',(200,200))
            ImageDraw.Draw(im).text((50,150),'H',font=f,fill=255,anchor='ls',stroke_width=stroke,stroke_fill=255)
            if (np.array(im).max(1)>60).sum()>=rh: break
        print('auto size',size)
    f=ImageFont.truetype(ttf,size,index=ttf_index)
    cyr=[0x401]+list(range(0x410,0x450))+[0x451]+([0x2014,0x2116] if extra_glyphs else [])
    LOOK={0x410:'A',0x412:'B',0x415:'E',0x41A:'K',0x41C:'M',0x41D:'H',0x41E:'O',0x420:'P',0x421:'C',0x422:'T',0x425:'X',
          0x430:'a',0x435:'e',0x43E:'o',0x440:'p',0x441:'c',0x443:'y',0x445:'x'}
    def render(c):
        ch=chr(c); W=size*3; im=Image.new('L',(W,baseline+size)); OX=size
        ImageDraw.Draw(im).text((OX,baseline),ch,font=f,fill=255,anchor='ls',stroke_width=stroke,stroke_fill=255)
        a=np.array(im)
        ys=np.where(a.max(1)>0)[0]; xs=np.where(a.max(0)>0)[0]
        if len(ys)==0: return dict(xo=0,yo=0,w=0,h=0,adv=int(round(f.getlength(ch))),kern=0xffff,lh=0,u0=0,u1=0,v0=0,v1=0),None
        a=a[ys[0]:ys[-1]+1,xs[0]:xs[-1]+1]
        xo0=xs[0]-OX; advf=f.getlength(ch)+stroke
        if hscale!=1.0:                      # condensed: narrower letters, same height
            nw=max(1,int(round(a.shape[1]*hscale)))
            a=np.array(Image.fromarray(a).resize((nw,a.shape[0]),Image.LANCZOS))
            xo0=xo0*hscale; advf=advf*hscale
        q=levels[np.abs(a[...,None].astype(int)-levels[None,None,:].astype(int)).argmin(-1)].astype(np.uint8)
        adv=int(round(advf))
        return dict(xo=int(round(xo0)),yo=int(ys[0]),w=q.shape[1],h=q.shape[0],adv=adv,kern=0xffff,lh=int(baseline-ys[0])),q
    if replace_all:
        for c in keep:
            if c==32: g=dict(glyphs[cmap[32]]); g['adv']=int(round(f.getlength(' ')*hscale)); items.append(([c],g,None)); continue
            g,q=render(c); items.append(([c],g,q))
    for c in cyr:
        if c in LOOK:
            k=[i for i,it in enumerate(items) if it[0][0]==ord(LOOK[c])][0]; items[k][0].append(c); continue
        g,q=render(c); items.append(([c],g,q))
    assert len(items)<=ng, (len(items),ng)
    # pack (shelf, tallest first, 1px gap)
    order=sorted(range(len(items)),key=lambda i:(-(items[i][2].shape[0] if items[i][2] is not None else 0),-(items[i][2].shape[1] if items[i][2] is not None else 0)))
    new=np.zeros_like(tex); sky=np.zeros(TW,int); GAP=1
    for i in order:
        codes,g,a=items[i]
        if a is None: continue
        hh,ww=a.shape; best=None
        for x in range(0,TW-ww+1):
            y=sky[x:x+ww+GAP if x+ww+GAP<=TW else TW].max()
            if y+hh<=TH and (best is None or y<best[1] or (y==best[1] and x<best[0])): best=(x,y)
        if best is None: raise SystemExit('atlas overflow at item %d (%s)'%(i,[hex(c) for c in codes]))
        x,y=best
        new[y:y+hh,x:x+ww]=a; sky[x:min(TW,x+ww+GAP)]=y+hh+GAP
        g['u0']=int(round((x+0.5)*US*64)); g['u1']=int(round((x+ww-0.5)*US*64))
        g['v0']=int(round((y+0.5)*VS*64)); g['v1']=int(round((y+hh-0.5)*VS*64))
    y=sky.max(); rowh=0
    print('atlas used rows', y+rowh, 'of', TH)
    # glyph records: items first, pad with copies of the space glyph
    recs=[it[1] for it in items]
    while len(recs)<ng: recs.append(dict([it for it in items if it[0][0]==32][0][1]))
    for k,g in enumerate(recs):
        struct.pack_into('<bbBB7H',d,gl_off+k*18,g['xo'],g['yo'],g['w'],g['h'],g['u0'],g['u1'],g['v0'],g['v1'],g['adv'],g['kern'],g['lh'])
    # code table
    newmap={}
    for k,(codes,_,_) in enumerate(items):
        for c in codes: newmap[c]=k
    ALIAS={0x2014:ord('-'),0x2116:ord('N'),0x2013:ord('-'),0x201C:ord('"'),0x201D:ord('"'),0x201E:ord('"'),0xA0:32}
    for c,t in ALIAS.items():
        if t in newmap: newmap.setdefault(c,newmap[t])
    for c in list(cmap):          # dropped codes -> base letter / fallback
        if c in newmap: continue
        base=unicodedata.normalize('NFD',chr(c))[0]
        newmap[c]=newmap.get(ord(base), newmap[ord('?')] if ord(base)>126 else newmap.get(ord(base),newmap[ord('?')]))
    for s in range(2048): struct.pack_into('<HH',d,cm_off+4*s,0xffff,0xffff)
    for c,k in newmap.items():
        s=h(c)
        while struct.unpack_from('<H',d,cm_off+4*s)[0]!=0xffff: s=(s+1)&2047; print('collision',hex(c))
        struct.pack_into('<HH',d,cm_off+4*s,c,k)
    b[D:D+dsz]=d; b[G:G+TW*TH]=new.tobytes()
    open(dst,'wb').write(b)
    if preview: Image.fromarray(new).resize((TW*3,TH*3),Image.NEAREST).save(preview)
    return len(items)

S='/mnt/user-data/uploads/Fonts/Supplemental/'
G='/mnt/user-data/uploads/Conker/game/dvddata/aid/font/'
O=_TDIR+'out/'
if __name__=='__main__':
    build(G+'ConkerFont/default.bin',O+'ConkerFont.bin',S+'PTSans.ttc',4,29,preview='/tmp/newatlas.png')
    build(G+'FrontendTitle/default.bin',O+'FrontendTitle.bin',S+'PTSerif.ttc',2,0,stroke=0,preview='/tmp/newatlas_ft.png',symbols=False,extra_glyphs=False,replace_all=True,hscale=0.82,drop=(ord('#'),ord('\\'),ord('&')))
