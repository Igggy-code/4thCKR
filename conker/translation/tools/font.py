import struct, numpy as np
def load(path):
    b=open(path,'rb').read()
    d=b[0x180:0x180+0x32d8]
    tex=np.frombuffer(b[len(b)-0xF000:],np.uint8).reshape(240,256)
    n=(len(d)-0x1186)//2
    t=struct.unpack_from('<%dH'%n,d,0x1186)
    cmap={}
    for i in range(0,2400,2):
        c,g=t[i],t[i+1]
        if c!=0xffff and g!=0xffff and g<199: cmap[c]=g
    glyphs=[]
    for k in range(199):
        o=0x388+k*18
        xo,yo,w,h=struct.unpack_from('<bbBB',d,o)
        u0,u1,v0,v1,adv,kern,lh=struct.unpack_from('<HHHHHHH',d,o+4)
        glyphs.append(dict(xo=xo,yo=yo,w=w,h=h,u0=u0/64,u1=u1/64,v0=v0/64,v1=v1/64,adv=adv,kern=kern,lh=lh))
    return d,tex,cmap,glyphs
def crop(tex,g):
    k=240/256
    x0,x1,y0,y1=int(g['u0']),int(round(g['u1'])),int(g['v0']*k),int(round(g['v1']*k))
    return tex[y0:max(y1,y0+1),x0:max(x1,x0+1)]
