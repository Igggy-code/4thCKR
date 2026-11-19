"""Quick, mechanical conversion of the fan all-caps text to normal case with
a few restored letters. Only for the first in-game font test; the edited
translation replaces it."""
import re
NAMES={'КОНКЕР','КОНКЕРА','КОНКЕРУ','КОНКЕРОМ','КОНКЕРЕ','БЕРРИ','ТЕДИЗ','ТЕДИЗОВ','ТЕДИЗЫ','ФРАНКИ','ДОН','ВОЛТ','КРИПЛСПАК','ГРЕГГ','БИРДИ','РЭНСОМ'}
def word(w):
    if any('a'<=c<='z' for c in w):                    # Latin word (Xbox Live): undo look-alike decoding
        return w.translate(str.maketrans('АВСЕНКМОРТХУ','ABCEHKMOPTXY'))
    lw=w.lower()
    lw=re.sub(r'([оеаыуяю])и$',r'\1й',lw)                 # КАКОИ -> какой
    lw=re.sub(r'([оеаыуяю])и(?=[^аеёиоуыэюя])',lambda m: m.group(1)+'й' if len(lw)>4 else m.group(0),lw) if False else lw
    if w in NAMES: lw=lw.capitalize()
    if w=='Я': lw='я'
    return lw
def convert(s):
    out=[]; cap=True
    for part in re.split(r'(\{[A-Z_]+\})',s):
        if part.startswith('{'):
            out.append(part)
            if part=='{PARAGRAPH}': cap=True
            continue
        res=[]
        for tok in re.split(r'([^\W\d_]+)',part):
            if tok and re.match(r'[^\W\d_]',tok):
                t=word(tok)
                if cap: t=t[:1].upper()+t[1:]; cap=False
                res.append(t)
            else:
                res.append(tok)
                if re.search(r'[.!?…]\s*$',tok) or re.search(r'[.!?…]["»)]?\s+$',tok): cap=True
        out.append(''.join(res))
    return ''.join(out)
if __name__=='__main__':
    for t in ['ПРИВЕТ, КОНКЕР! КАКОИ ХОРОШИИ ДЕНЬ.{PARAGRAPH}ДА, Я ЗНАЮ...','Хbox Live ВКЛЮЧЕН']: print(convert(t))
