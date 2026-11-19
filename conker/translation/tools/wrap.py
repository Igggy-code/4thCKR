import os as _os
_TDIR=_os.path.normpath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))+'/'
"""Line-wrap Russian speech bubbles like the English ones.
{PARAGRAPH} starts a new bubble (one voice line); {PMARKER} is a manual line
break inside a bubble. Each Russian bubble is wrapped to the widest line of the
matching English bubble (measured with the game font), balanced so lines are
even."""
import re, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from fontrender import Font
EN_FONT=Font('/mnt/user-data/uploads/Conker/game/dvddata/aid/font/ConkerFont/default.bin')
RU_FONT=Font(_TDIR+'out/ConkerFont.bin')
ZERO={'{RED}','{NORMAL}','{THROB}','{BLUE}','{GREEN}'}
TAG=re.compile(r'\{[A-Z_]+\}')
def width(s, f):
    w=0
    for part in re.split(r'(\{[A-Z_]+\})', s):
        if not part: continue
        if TAG.fullmatch(part):
            if part not in ZERO: w+=24
            continue
        for ch in part:
            g=f.glyph(ord(ch)) or f.glyph(ord('?')); w+=g[2]
    return w
def wrap_bubble(text, W):
    words=text.split(' ')
    words=[w for w in words if w!='']
    if not words: return text
    sw=width(' ',RU_FONT)
    ws=[width(w,RU_FONT) for w in words]
    n=len(words)
    def lw(i,j): return sum(ws[i:j])+sw*(j-i-1)
    total=lw(0,n)
    if total<=W: return ' '.join(words)
    import math
    best=None
    for lines in range(max(2,math.ceil(total/W)), n+1):
        # DP: min sum of squared (W - line) with all lines <= W*1.02 (single long word allowed)
        INF=float('inf'); dp=[[INF]*(n+1) for _ in range(lines+1)]; bk=[[0]*(n+1) for _ in range(lines+1)]
        dp[0][0]=0
        for k in range(1,lines+1):
            for j in range(1,n+1):
                for i in range(j-1,-1,-1):
                    L=lw(i,j)
                    if L>W*1.02 and j-i>1: break
                    c=dp[k-1][i]
                    if c==INF: continue
                    c+= (max(0,W-L))**2 if k<lines else 0
                    if c<dp[k][j]: dp[k][j]=c; bk[k][j]=i
        if dp[lines][n]<INF:
            out=[]; j=n
            for k in range(lines,0,-1):
                i=bk[k][j]; out.append(' '.join(words[i:j])); j=i
            # rebalance last-line-free cost: fine
            return '{PMARKER}'.join(reversed(out))
    return ' '.join(words)
def en_bubble_widths(en):
    res=[]
    for b in en.split('{PARAGRAPH}'):
        lines=b.split('{PMARKER}')
        res.append((len(lines), max(width(l.strip(),EN_FONT) for l in lines)))
    return res
def split_exact(text, n):
    """Split a bubble into exactly n segments ({PMARKER} is a phrase marker the
    game also times the bubble by, so the count must match the English one),
    balanced by pixel width."""
    words=[w for w in text.split(' ') if w!='']
    if n<=1 or len(words)<=1: return ' '.join(words)
    n=min(n,len(words))
    sw=width(' ',RU_FONT); ws=[width(w,RU_FONT) for w in words]
    def lw(i,j): return sum(ws[i:j])+sw*(j-i-1)
    INF=float('inf'); m=len(words)
    dp=[[INF]*(m+1) for _ in range(n+1)]; bk=[[0]*(m+1) for _ in range(n+1)]
    dp[0][0]=0
    for k in range(1,n+1):
        for j in range(1,m+1):
            for i in range(k-1,j):
                c=max(dp[k-1][i], lw(i,j))
                if c<dp[k][j]: dp[k][j]=c; bk[k][j]=i
    out=[]; j=m
    for k in range(n,0,-1):
        i=bk[k][j]; out.append(' '.join(words[i:j])); j=i
    return '{PMARKER}'.join(reversed(out))
def wrap(ru, en):
    """ru: text with {PARAGRAPH}; every bubble gets exactly as many {PMARKER}
    phrase markers as the English bubble (the game wraps long lines itself)."""
    if '{PMARKER}' not in en:
        return ru
    enb=en.split('{PARAGRAPH}'); rub=ru.split('{PARAGRAPH}')
    out=[]
    for k,b in enumerate(rub):
        n=enb[k].count('{PMARKER}')+1 if k<len(enb) else 1
        b=b.replace('{PMARKER}',' ')
        out.append(split_exact(b.strip(), n))
    return '{PARAGRAPH}'.join(out)
if __name__=='__main__':
    en="I thought i told you{PMARKER}to leave town.{PMARKER}But seein' as you're here,{PMARKER}anyways,{PMARKER}we have a little job for you!"
    print(en_bubble_widths(en))
    print(wrap("Я же вроде велел тебе убираться из города. Но раз уж ты всё равно здесь, есть для тебя работёнка!",en))
