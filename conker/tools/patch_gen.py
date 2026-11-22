#!/usr/bin/env python3
"""Port-specific edits applied to freshly generated code (src/recomp/gen).

The recompiler output is produced on the builder's machine from their own
default.xbe and never stored in this repository. A few places in it need
changes for the Android port; they are described here by function address
and label only, and applied after every generation:

  * GPU register access: D3D's interrupt and push-buffer code reads and
    writes NV2A registers through the GPU aperture. In the functions listed
    in GPU_FUNCS every 32-bit access goes through GPU_R32/GPU_W32 (defined in
    recomp_types.h), which call the device model directly for register
    addresses and fall back to plain memory otherwise -- instead of a SIGSEGV,
    decode and emulate per access.
  * The push-buffer callback (sub_0053AD90) opens its destination pages
    before copying into GPU-visible memory (no fault per page).
  * D3D_BlockOnTime's tight polling loop sleeps on the GPU semaphore instead
    of spinning (conker_bot_spin in recomp_manual.c).
  * The flip command's presentation interval goes through
    conker_flip_interval (frame-rate modes, developer setting).
  * Two entries of the CRT's x87 dispatch table that the analysis misses are
    written by hand (src/recomp_extra_seeds.c); they are declared and added
    to the dispatch table here.

usage: patch_gen.py <gen-dir>
"""
import glob
import os
import re
import sys

GPU_FUNCS = [int(x, 16) for x in """
3f2c00 5380e0 538c50 53a330 53c0a0 53c120 53c450 53c500 53c630 53d6c0 53d700
53e5d0 53eef0 5401e5 540370 540372 5403b3 540434 540554 54061d 54067b 5406ab
54085f 540a4c 540b13 540b79 540c47 540ca0 540dc0 540ea0 540f70 541080 5411d0
541280 541410 541660 541718 541796 541881 541af0 541c50 541fd0 5421d0 5423c0
542790 542830 542920 542ad0 542bf0 542f60 543200 5433b0 543500 543550 5439b0
543f53 5440b6 5441b6 544285 5442bb 5442eb 54435c 5445d8 544613 544733 54476a
5447ef 5449e7 544b81 544bb1 545670""".split()]

PREP_WRITE = '''/* port: the push-buffer callback copies blocks into memory the GPU reads,
 * which is write-protected for dirty tracking: open the destination first
 * (pages unprotected and marked dirty) instead of a fault per page. */
extern void nv2a_host_prepare_write(uint64_t addr, uint64_t size);
static inline void conker_prep_write(uint32_t va, uint64_t n)
{
    if (!n) return;
    if (va >= 0x80000000u && va < 0x88000000u) nv2a_host_prepare_write(va - 0x80000000u, n);
    else if (va >= 0xF0000000u && va < 0xF8000000u) nv2a_host_prepare_write(va - 0xF0000000u, n);
}
'''

FUNC_RE = re.compile(r'^((?:static )?void (sub_([0-9A-F]{8})\w*)\(void\)\n\{.*?\n\}\n)', re.S | re.M)


def args_span(s, i):
    """s[i] == '(' -> index of the matching ')'."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced parentheses')


def gpu_convert(text):
    """MEM32(a) = v;  ->  GPU_W32(a, v);   other MEM32(a)  ->  GPU_R32(a)."""
    res, i = [], 0
    while True:
        k = text.find('MEM32(', i)
        if k < 0:
            res.append(text[i:])
            return ''.join(res)
        # not part of a longer identifier (SMEM32, ...)
        if k > 0 and (text[k - 1].isalnum() or text[k - 1] == '_'):
            res.append(text[i:k + 6])
            i = k + 6
            continue
        e = args_span(text, k + 5)
        res.append(text[i:k])
        addr = gpu_convert(text[k + 6:e])
        m = re.match(r'\s*=(?!=)', text[e + 1:])
        if m:
            v0 = e + 1 + m.end()
            v1 = stmt_end(text, v0)
            res.append('GPU_W32(%s, %s)' % (addr, gpu_convert(text[v0:v1].strip())))
            i = v1
        else:
            res.append('GPU_R32(%s)' % addr)
            i = e + 1


def stmt_end(s, i):
    """Index of the ';' (or unmatched ')') that ends the expression at s[i:]."""
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c == '(':
            depth += 1
        elif c == ')':
            if depth == 0:
                return j
            depth -= 1
        elif c in ';,' and depth == 0:
            return j
    raise ValueError('unterminated store')


def patch_function(addr, name, body):
    if addr in GPU_FUNCS:
        body = gpu_convert(body)
    if addr == 0x0053AD90:
        body, n = re.subn(r'(\{ uint64_t _n = \(uint64_t\)ecx \* [14]u;\n)',
                          r'\1    conker_prep_write(edi, _n);\n', body)
        assert n == 2, 'sub_0053AD90: copy blocks not found'
        body = PREP_WRITE + body
    if 'loc_0053C2C0: ;' in body:
        body, n = re.subn(r'(\n)(\nloc_0053C2C0: ;)',
                          r'\n    { extern void conker_bot_spin(uint32_t, uint32_t, uint32_t); '
                          r'conker_bot_spin(edx, esi, eax); }\n\2', body, count=1)
        assert n == 1, 'loc_0053C2C0 not found'
    if addr == 0x00541410:
        i = body.index('loc_00541434: ;')
        j = body.index('ebp = ebp & 7;\n', i) + len('ebp = ebp & 7;\n')
        body = (body[:j] + '    { extern int conker_flip_interval(int); '
                'ebp = (uint32_t)conker_flip_interval((int)ebp); }\n' + body[j:])
    return body


EXTRA_ENTRIES = [0x0047B334, 0x0047B3D6]


def add_extra_entries(gen):
    hdr = os.path.join(gen, 'recomp_funcs.h')
    h = open(hdr).read()
    decls = ''.join('void sub_%08X(void);\n' % a for a in EXTRA_ENTRIES
                    if 'void sub_%08X(void);' % a not in h)
    if decls:
        i = h.rindex('\n#endif')
        open(hdr, 'w').write(h[:i] + '\n/* port: hand-written, src/recomp_extra_seeds.c */\n' + decls + h[i:])
    dsp = os.path.join(gen, 'recomp_dispatch.c')
    d = open(dsp).read()
    start = d.index('g_recomp_table[] = {\n') + len('g_recomp_table[] = {\n')
    end = d.index('\n};', start)
    rows = d[start:end].split('\n')
    have = {int(r.split('0x')[1][:8], 16) for r in rows if '0x' in r}
    for a in EXTRA_ENTRIES:
        if a not in have:
            rows.append('    { 0x%08Xu, (recomp_func_t)sub_%08X },' % (a, a))
    rows.sort(key=lambda r: int(r.split('0x')[1][:8], 16))
    d = d[:start] + '\n'.join(rows) + d[end:]
    d = re.sub(r'g_recomp_table_size = \d+;',
               'g_recomp_table_size = %d;' % sum('0x' in r for r in rows), d)
    open(dsp, 'w').write(d)


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else 'src/recomp/gen'
    files = sorted(glob.glob(os.path.join(gen, 'recomp_0*.c')))
    if not files:
        sys.exit('no generated files in %s' % gen)
    done = set()
    for path in files:
        src = open(path).read()
        if 'port-edits-applied' in src:
            continue

        def repl(m):
            addr = int(m.group(3), 16)
            done.add(addr)
            return patch_function(addr, m.group(2), m.group(1))

        new = FUNC_RE.sub(repl, src)
        new = '/* port-edits-applied (tools/patch_gen.py) */\n' + new
        open(path, 'w').write(new)
    missing = [hex(a) for a in GPU_FUNCS + [0x0053AD90, 0x00541410] if a not in done]
    if missing:
        sys.exit('functions not found in the generated code: %s' % ' '.join(missing))
    add_extra_entries(gen)
    print('patched %d files' % len(files))


if __name__ == '__main__':
    main()
