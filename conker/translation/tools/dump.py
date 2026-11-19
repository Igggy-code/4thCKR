import os as _os
_TDIR=_os.path.normpath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))+'/'
import json,sys,re
rows=json.load(open(_TDIR+'strings_fan.json'))
def norm(s):
    s=re.sub(r'\s*\{PMARKER\}\s*',' ',s); s=s.replace('{PARAGRAPH}',' ¶ ').replace('\n','\\n')
    return re.sub(r' +',' ',s).strip()
f=sys.argv[1]; lo=int(sys.argv[2]) if len(sys.argv)>2 else 0; hi=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
sel=[r for r in rows if r['file']==f][lo:hi]
for r in sel: print('%d\t%s'%(r['id'],norm(r['en'])))
