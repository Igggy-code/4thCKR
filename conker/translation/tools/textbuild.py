"""Rebuild a Conker CAFF text bank (.../text/English/<name>/default.bin) with
strings of any length. Strings live inside the LSBL block; everything after them
(speech-cue chunks, debug path, relocation table) is shifted and the few offsets
that point past the strings are fixed up. Relocations themselves all point
before LSBL, so they never change."""
import struct
def build(src_bytes, strings, same_size=False):
    a=bytearray(src_bytes); D=0x180
    dsz=struct.unpack_from('<I',a,0x50)[0]; assert len(a)==D+dsz
    d=a[D:]
    L=d.find(b'LSBL'); h=list(struct.unpack_from('<8I',d,L))
    n=struct.unpack_from('<I',d,L+0x20)[0]
    ents=[struct.unpack_from('<HI',d,L+0x24+6*k) for k in range(n+1)]
    base=L+0x24+6*(n+1)
    assert len(strings)==n
    old_end=L+h[7]+0x1c       # first byte after the string area (incl. pad); h[4]==h[7]+0x1c or 0
    # new string area
    body=bytearray(); offs=[]
    for s in strings:
        offs.append(len(body)//2); body+= (s+'\x00').encode('utf-16le')
    sent=len(body)//2
    old_body_len=old_end-base
    old_sent=ents[n][1]
    pad=old_body_len-old_sent*2          # trailing bytes after sentinel in original (usually 2)
    body+=b'\x00'*pad
    delta=len(body)-old_body_len
    if same_size and delta<0:              # pad the string area: the file keeps its exact size
        body+=b'\x00'*(-delta); delta=0
    if delta%4: body+=b'\x00'*(4-delta%4); delta=len(body)-old_body_len
    tab=bytearray()
    for k in range(n): tab+=struct.pack('<HI',ents[k][0],offs[k])
    tab+=struct.pack('<HI',ents[n][0],sent)
    f34=struct.unpack_from('<I',a,0x34)[0]
    nd=bytearray(d[:L+0x24])+tab+body+d[old_end:]
    for k in (4,5,6):
        if h[k]: h[k]+=delta
    h[7]+=delta
    struct.pack_into('<8I',nd,L,*h)
    # trailer: at f34: u32 len, u32 0, path\0, u32 1, u32 0, u32 end-of-content (~f34-2)
    nf34=f34+delta
    plen=struct.unpack_from('<I',nd,nf34)[0]
    q=nf34+8+plen
    m=struct.unpack_from('<I',nd,q+8)[0]
    fixed = f34-4 <= m <= f34
    if fixed: struct.pack_into('<I',nd,q+8,m+delta)
    assert fixed, 'trailer marker not found'
    out=bytearray(a[:D])+nd
    struct.pack_into('<I',out,0x34,nf34)
    struct.pack_into('<I',out,0x50,len(nd)); struct.pack_into('<I',out,0x64,len(nd))
    return bytes(out)
