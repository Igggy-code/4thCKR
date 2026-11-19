import os as _os
_TDIR=_os.path.normpath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))+'/'
"""ru/<File>.tsv (id<TAB>text, ¶ = new bubble, \\n literal) -> validated, wrapped
'final' strings in strings_final.json."""
import json,sys,re,glob,os,collections
sys.path.insert(0,os.path.dirname(__file__))
from wrap import wrap, width, RU_FONT, EN_FONT
T=_TDIR
rows=json.load(open(T+'strings_fan.json'))
from fan2case import convert
FAN={(r['file'],r['id']):r['ru'] for r in rows}
fin={}
if os.path.exists(T+'strings_final.json'): fin={(r['file'],r['id']):r['final'] for r in json.load(open(T+'strings_final.json')) if r.get('final')}
TAGS=re.compile(r'\{(?!PMARKER|PARAGRAPH)[A-Z_]+\}|%[sdiu]')
problems=[]
ENALL={}
for r in rows: ENALL.setdefault(r['file'],{})[r['id']]=r['en']
SEEN=collections.defaultdict(set)
for p in sorted(glob.glob(T+'ru/*.tsv')):
    name=os.path.basename(p)[:-4]
    for line in open(p,encoding='utf-8'):
        line=line.rstrip('\n')
        if not line.strip() or line.startswith('#'): continue
        if line.startswith('@'):
            name=line[1:].strip()
            if name.endswith('+EN'):          # ids not listed keep the English (numbers, debug)
                name=name[:-3].strip()
                for i,e in ENALL[name].items():
                    if (name,i) not in fin or True: fin.setdefault((name,i),e)
                    SEEN[name].add(i)
                continue
            if name.endswith('=EN'):          # keep the English file as is (credits: names)
                name=name[:-3].strip()
                for i,e in ENALL[name].items(): fin[(name,i)]=e; SEEN[name].add(i)
            continue
        en=ENALL.get(name,{}); seen=SEEN[name]
        sid,txt=(line.split('\t',1)+[''])[:2]; sid=int(sid)
        if sid not in en: problems.append((name,sid,'unknown id')); continue
        seen.add(sid)
        e=en[sid]
        if txt.strip()=='=EN':           # keep the English line (unreachable in this port: Xbox Live)
            fin[(name,sid)]=e; continue
        if txt.strip()=='=FAN':          # song lyrics: the existing fan lines, case-converted only
            fin[(name,sid)]=convert(FAN[(name,sid)]).rstrip(' '); continue
        txt=txt.replace('\\n','\n')
        txt=re.sub(r'\s*¶\s*','{PARAGRAPH}',txt)
        if e.strip()=='' : txt=e
        if collections.Counter(TAGS.findall(txt))!=collections.Counter(TAGS.findall(e)):
            problems.append((name,sid,'tags %s vs %s'%(TAGS.findall(txt),TAGS.findall(e))))
        if txt.count('{PARAGRAPH}')!=e.count('{PARAGRAPH}'):
            problems.append((name,sid,'bubbles %d vs %d'%(txt.count('{PARAGRAPH}'),e.count('{PARAGRAPH}'))))
        fin[(name,sid)]=wrap(txt,e)
for name,seen in SEEN.items():
    miss=[i for i in ENALL[name] if i not in seen]
    if miss: problems.append((name,None,'missing ids %s'%miss[:10]))
print('files done:',len(SEEN),'of',len(ENALL),'; todo:',sorted(set(ENALL)-set(SEEN)))
out=[dict(file=f,id=i,final=t) for (f,i),t in fin.items()]
json.dump(out,open(T+'strings_final.json','w'),ensure_ascii=False,indent=0)
for pr in problems: print('!!',*pr)
print('final strings',len(out))
