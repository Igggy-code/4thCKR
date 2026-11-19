import os as _os
_TDIR=_os.path.normpath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))+'/'
import sys,os,json,glob
sys.path.insert(0,os.path.dirname(__file__))
from textbuild import build
from text import parse
from fan2case import convert
GAME='/mnt/user-data/uploads/Conker/game'
def main(out, strings_json=None):
    rows=json.load(open(_TDIR+'strings_fan.json'))
    fin={(r['file'],r['id']):r['final'] for r in json.load(open(strings_json or _TDIR+'strings_final.json'))}
    for r in rows: r['final']=fin.get((r['file'],r['id']))
    by={}
    for r in rows: by.setdefault(r['file'],{})[r['id']]=r
    for name,m in by.items():
        src=GAME+'/dvddata/aid/text/English/%s/default.bin'%name
        P=parse(src); S=[]
        for lid,en in P['strs']:
            r=m.get(lid)
            S.append((r['final'] if r.get('final') is not None else convert(r['ru']).rstrip(' ')) if r else en)
        dst=out+'/dvddata/aid/text/English/%s/default.bin'%name
        os.makedirs(os.path.dirname(dst),exist_ok=True)
        data=build(open(src,'rb').read(),S,same_size=True)
        orig=os.path.getsize(src)
        if len(data)!=orig: raise SystemExit('%s: %d bytes > original %d (the game keeps text banks in fixed buffers)'%(name,len(data),orig))
        open(dst,'wb').write(data)
    for f in ('ConkerFont','FrontendTitle'):
        dst=out+'/dvddata/aid/font/%s/default.bin'%f; os.makedirs(os.path.dirname(dst),exist_ok=True)
        open(dst,'wb').write(open(_TDIR+'out/%s.bin'%f,'rb').read())
    print('files',len(by)+2)
if __name__=='__main__': main(*sys.argv[1:])
