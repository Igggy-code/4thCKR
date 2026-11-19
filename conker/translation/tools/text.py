import struct
def parse(path):
    b=open(path,'rb').read()
    i=b.find(b'LSBL')
    hdr=struct.unpack_from('<8I',b,i)
    n=struct.unpack_from('<I',b,i+0x20)[0]
    o=i+0x24; ents=[]
    for k in range(n+1):
        lid,off=struct.unpack_from('<HI',b,o); ents.append((lid,off)); o+=6
    base=o
    strs=[]
    for k in range(n):
        lid,off=ents[k]; end=ents[k+1][1]
        raw=b[base+off*2:base+end*2]
        strs.append((lid,raw.decode('utf-16le').rstrip('\x00')))
    # key names: utf-16 strings before LSBL
    return dict(hdr=hdr,base=base,n=n,ents=ents,strs=strs,lsbl=i,size=len(b))
RU={'A':'А','B':'В','C':'С','D':'Д','E':'Е','F':'Ы','G':'Ш','H':'Н','J':'П','K':'К','L':'Л','M':'М',
    'N':'И','O':'Ф','P':'Р','Q':'Э','R':'Я','S':'Ь','T':'Т','U':'Ю','V':'Ц','W':'Ж','X':'Х','Y':'У',
    'Z':'Г','0':'О','3':'З','4':'Ч','6':'Б'}
import re
LOW={'b':'Ь','o':'О','a':'А','e':'Е','x':'Х','c':'С','p':'Р','y':'У','k':'К','m':'М','t':'Т','h':'Н'}
def decode_word(w):
    lows=sum(1 for ch in w if 'a'<=ch<='z')
    if lows>=2: return w            # an English word left as is (Xbox Live, Move it out)
    return ''.join(RU.get(ch, LOW.get(ch, ch)) for ch in w)
def decode(s):
    out=[]
    for part in re.split(r'(\{[A-Z_]+\})', s):
        if part.startswith('{'): out.append(part); continue
        out.append(re.sub(r'[A-Za-z0-9]+', lambda m: decode_word(m.group(0)), part))
    return ''.join(out)
def decode_old(s):
    out=[];i=0
    while i<len(s):
        if s[i]=='{':
            j=s.find('}',i)
            if j>0: out.append(s[i:j+1]); i=j+1; continue
        out.append(RU.get(s[i],s[i])); i+=1
    return ''.join(out)
