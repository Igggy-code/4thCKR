import struct, numpy as np
from PIL import Image
def h(c): return (c + (c >> 5) + 2) & 2047
class Font:
    def __init__(s,path):
        b=open(path,'rb').read()
        dsz=struct.unpack_from('<I',b,0x50)[0]; gsz=struct.unpack_from('<I',b,0x78)[0]
        d=b[0x180:0x180+dsz]; s.d=d
        s.cm=struct.unpack_from('<I',d,0x344)[0]; s.go=struct.unpack_from('<I',d,0x370)[0]
        TW,TH=struct.unpack_from('<HH',d,0x380); s.VS=256/TH; s.US=256/TW
        s.tex=np.frombuffer(b[len(b)-gsz:len(b)-gsz+TW*TH],np.uint8).reshape(TH,TW)
    def glyph(s,c):
        k=h(c)
        while True:
            cc,g=struct.unpack_from('<HH',s.d,s.cm+4*k)
            if cc==c: break
            if cc==0xffff: return None
            k=(k+1)&2047
        xo,yo,w,hh,u0,u1,v0,v1,adv,kern,lh=struct.unpack_from('<bbBB7H',s.d,s.go+18*g)
        x=int(round(u0/64/s.US-0.5)); y=int(round(v0/64/s.VS-0.5))
        return xo,yo,adv,(s.tex[y:y+hh,x:x+w] if w and hh else None)
    def line(s,t,W=None,H=44):
        gs=[s.glyph(ord(c)) or s.glyph(ord('?')) for c in t]
        W=W or sum(g[2] for g in gs)+8
        im=np.zeros((H,W),np.uint8); x=4
        for xo,yo,adv,a in gs:
            if a is not None:
                hh,ww=a.shape; X=x+xo
                if 0<=X and X+ww<=W and yo>=0 and yo+hh<=H: im[yo:yo+hh,X:X+ww]=np.maximum(im[yo:yo+hh,X:X+ww],a)
            x+=adv
        return im,x
