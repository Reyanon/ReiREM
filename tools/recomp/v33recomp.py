#!/usr/bin/env python3
"""V33 static recompiler (M6, docs/RULES.md 7-1, ADR-008)

Input: coverage from harness --cover (v33_cover.txt, may be several) and a V33 address space image (v33_image.bin)
Output: C++ code included by nec.cpp (src/recomp/generated/v33_rc.inc)

Generated code contains constants derived from ROM bytes, so it is never committed (docs/RULES.md 6.1).
Users generate it from their own ROM with tools/recomp/gen.sh.

Design
- Only instruction start addresses seen in coverage are compiled. Everything else runs in the interpreter (fallback).
- Nearby instructions are grouped into one region function. Each instruction has a label; entry is switch(pc).
- State after each instruction (icount, prefetch, ip) must match the interpreter (hash match).
  Between instructions, if the interpreter loop would do something else (icount<=0, stop_run, no_interrupt, pending IRQ),
  return to the loop.
- Two instruction implementations:
  * generic: emulate the opcode fetch, then call the existing handler directly (identical to the interpreter by definition)
  * specialized: copy the handler body from necinstr.c verbatim and make FETCH read instruction bytes as constants.
    ip increments and prefetch math match the original byte by byte; the compiler folds ModRM branches, register tables and EA
    into constants. The body is wrapped in a lambda so a return inside the handler keeps its original meaning.
"""
import argparse
import collections
import os
import re
import sys

IMAGE_END = 0xA0000  # fixed ROM area (0xa0000+ is banked)
PREFIXES = {0x26: 'DS1', 0x2E: 'PS', 0x36: 'SS_', 0x3E: 'DS0'}
REP_PREFIXES = {0xF2, 0xF3, 0x64, 0x65}  # includes repnc/repc - the handler executes the next instruction internally
NEC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'core', 'cpu', 'nec')

# ---------------------------------------------------------------------------
# Length decoder (in bytes FETCHed by this core)

MODRM_OPS = set()
for base in range(0x00, 0x40, 8):
    MODRM_OPS |= {base, base + 1, base + 2, base + 3}
MODRM_OPS |= {0x62, 0x69, 0x6B} | set(range(0x80, 0x90)) | {0xC0, 0xC1, 0xC4, 0xC5, 0xC6, 0xC7}
MODRM_OPS |= set(range(0xD0, 0xD4)) | set(range(0xD8, 0xE0)) | {0xF6, 0xF7, 0xFE, 0xFF}

IMM8_OPS = {0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x6A, 0xA8, 0xCD, 0xD4, 0xD5,
            0xE4, 0xE5, 0xE6, 0xE7, 0xEB} | set(range(0x70, 0x80)) | set(range(0xB0, 0xB8)) | set(range(0xE0, 0xE4))
IMM16_OPS = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0x68, 0xA9, 0xC2, 0xCA, 0xE8, 0xE9,
             0xA0, 0xA1, 0xA2, 0xA3} | set(range(0xB8, 0xC0))


def modrm_len(m):
    mod, rm = m >> 6, m & 7
    if mod == 3:
        return 1
    if mod == 0:
        return 3 if rm == 6 else 1
    return 2 if mod == 1 else 3


def insn_len_at(img, pc):
    """Length of the instruction at pc (including prefixes). None if unknown"""
    i = pc
    while img[i] in PREFIXES or img[i] in REP_PREFIXES:
        i += 1
        if i - pc > 4:
            return None
    op = img[i]
    n = i - pc + 1
    if op == 0x0F:
        op2 = img[i + 1]
        n += 1
        if 0x10 <= op2 <= 0x17 or op2 in (0x28, 0x2A):
            n += modrm_len(img[i + 2])
        elif 0x18 <= op2 <= 0x1F:
            n += modrm_len(img[i + 2]) + 1
        elif op2 in (0x31, 0x33, 0xE0, 0xF0, 0xFF):
            n += 1
        return n
    if op in MODRM_OPS:
        m = img[i + 1]
        n += modrm_len(m)
        reg = (m >> 3) & 7
        if op in (0x80, 0x82, 0x83, 0xC0, 0xC1, 0xC6, 0x6B):
            n += 1
        elif op in (0x81, 0xC7, 0x69):
            n += 2
        elif op == 0xF6 and reg in (0, 1):
            n += 1
        elif op == 0xF7 and reg in (0, 1):
            n += 2
        return n
    if op in IMM8_OPS:
        return n + 1
    if op in IMM16_OPS:
        return n + 2
    if op in (0x9A, 0xEA):
        return n + 4
    if op == 0xC8:
        return n + 3
    return n


def s8(v):
    return v - 256 if v >= 128 else v


def s16(v):
    return v - 65536 if v >= 32768 else v


def static_targets(img, pc, ln):
    """Statically known next addresses (branch targets + fallthrough), and whether fallthrough is possible"""
    i = pc
    while img[i] in PREFIXES or img[i] in REP_PREFIXES:
        i += 1
    op = img[i]
    nxt = pc + ln
    if 0x70 <= op <= 0x7F or 0xE0 <= op <= 0xE3:
        return [nxt + s8(img[i + 1]), nxt], True
    if op == 0xEB:
        return [nxt + s8(img[i + 1])], False
    if op == 0xE9:
        return [nxt + s16(img[i + 1] | img[i + 2] << 8)], False
    if op == 0xE8:  # call: target (return address is dynamic via ret)
        return [nxt + s16(img[i + 1] | img[i + 2] << 8)], False
    if op in (0xC2, 0xC3, 0xCA, 0xCB, 0xCF, 0xEA, 0x9A, 0xCC, 0xCD, 0xCE, 0xF4):
        return [], False
    if op == 0xFF and ((img[i + 1] >> 3) & 7) in (2, 3, 4, 5):
        return [], False
    return [nxt], True


# ---------------------------------------------------------------------------
# Parse the original sources (handler bodies, EA expressions, macros using GetEA)

def read_src(name):
    return open(os.path.join(NEC_DIR, name), encoding='utf-8', errors='replace').read()


def strip_comments(t):
    out = []
    i = 0
    while i < len(t):
        if t.startswith('//', i):
            j = t.find('\n', i)
            i = len(t) if j < 0 else j
        elif t.startswith('/*', i):
            i = t.index('*/', i) + 2
            out.append(' ')
        else:
            out.append(t[i])
            i += 1
    return ''.join(out)


def match_brace(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
    raise ValueError('unbalanced')


def parse_handlers():
    """necinstr.c OP(0xNN, name) { body } -> {opcode: body}"""
    src = strip_comments(read_src('necinstr.c'))
    h = {}
    for m in re.finditer(r'^OP\(\s*0x([0-9a-fA-F]{2})\s*,\s*(\w+)\s*\)\s*\{', src, re.M):
        b0 = m.end() - 1
        b1 = match_brace(src, b0)
        h[int(m.group(1), 16)] = src[b0 + 1:b1]
    return h


def parse_defines(name, pred):
    """#defines of name where pred(text) holds -> [(macro, text)]"""
    lines = strip_comments(read_src(name)).split('\n')
    out = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('#define'):
            blk = [lines[i]]
            while blk[-1].rstrip().endswith('\\'):
                i += 1
                blk.append(lines[i])
            text = '\n'.join(x.rstrip() for x in blk)
            mname = text.split()[1].split('(')[0]
            if pred(text):
                out.append((mname, text))
        i += 1
    return out


def parse_ea():
    """necea.h EA_mrm function -> {modrm(0..191): C expression}"""
    src = read_src('necea.h')
    fns = {}
    for m in re.finditer(r'static unsigned (EA_\d+)\(nec_state_t \*nec_state\) \{(.*?)\}', src):
        stmts = [x.strip() for x in m.group(2).split(';') if x.strip()]
        assert stmts[-1] == 'return EA'
        fns[m.group(1)] = '(' + ', '.join('(' + x + ')' for x in stmts[:-1]) + ', EA)'
    return {mrm: fns['EA_%d0%d' % (mrm >> 6, mrm & 7)] for mrm in range(192)}


GETEA_CALL = '(*GetEA[ModRM])(nec_state)'
PREFIX_CALL = 'nec_instruction[fetchop(nec_state)](nec_state);'
NO_SPEC = {0x9B, 0xF4}  # wait(ip--), hlt - rare and loop-dependent, so generic
SPECIAL_MACROS = lambda t: 'GetEA' in t or 'chip_type' in t


def spec_body(img, i, handlers, depth=0):
    """Handler body for opcode img[i]. For a segment prefix, inline the next instruction's body"""
    op = img[i]
    body = handlers[op].replace(GETEA_CALL, 'RC_EA()')
    if op in PREFIXES and depth < 3:
        assert PREFIX_CALL in body
        inner = spec_body(img, i + 1, handlers, depth + 1)
        body = body.replace(PREFIX_CALL, 'fetchop(nec_state); [&]() {' + inner + '}();')
    return body


def modrm_of(img, pc):
    i = pc
    while img[i] in PREFIXES:
        i += 1
    op = img[i]
    if op == 0x0F:
        op2 = img[i + 1]
        if 0x10 <= op2 <= 0x1F or op2 in (0x28, 0x2A):
            return img[i + 2]
        return None
    if op in MODRM_OPS:
        return img[i + 1]
    return None


def gen_spec(img, pc, ln, handlers, ea):
    i = pc
    while img[i] in PREFIXES:
        i += 1
    if img[i] in NO_SPEC or img[i] in REP_PREFIXES:
        return None
    m = modrm_of(img, pc)
    lines = []
    lines.append('#undef RC_EA')
    lines.append('#define RC_EA() ' + (ea[m] if m is not None and m < 0xC0 else '(EA)'))
    lines.append('static const UINT8 rc_b[] = {%s}; int rc_i = 0; UINT32 rc_lo; (void)rc_lo;' %
                 ', '.join('0x%02x' % b for b in img[pc:pc + ln]))
    body = spec_body(img, pc, handlers).replace('\n', ' ')
    lines.append('fetchop(nec_state); [&]() {' + body + '}();')
    return lines


def gen_generic(img, pc, ln):
    """Emulated opcode fetch + direct handler call. Prefix handlers (i_es etc.) execute the next instruction internally"""
    return [
        'prefetch(nec_state); nec_state->ip++;',
        'nec_instruction[0x%02x](nec_state);' % img[pc],
    ]


def preamble():
    """Macro overrides for generated regions (restored in the postamble)"""
    W = []
    wn = ['AW', 'CW', 'DW', 'BW', 'SP', 'BP', 'IX', 'IY']
    bn = ['AL', 'CL', 'DL', 'BL', 'AH', 'CH', 'DH', 'BH']
    W.append('static const struct { struct { WREGS w[256]; BREGS b[256]; } reg; '
             'struct { WREGS w[256]; BREGS b[256]; } RM; } rc_Mod_RM = {')
    W.append('\t{ { %s }, { %s } },' % (', '.join(wn[(i >> 3) & 7] for i in range(256)),
                                        ', '.join(bn[(i >> 3) & 7] for i in range(256))))
    W.append('\t{ { %s }, { %s } } };' % (', '.join(wn[i & 7] if i >= 0xC0 else 'AW' for i in range(256)),
                                          ', '.join(bn[i & 7] if i >= 0xC0 else 'AL' for i in range(256))))
    W.append('#define Mod_RM rc_Mod_RM')
    W.append('#define RC_EA() (EA) // unused branch when ModRM >= 0xc0')
    W.append('#undef FETCH')
    W.append('#define FETCH() (prefetch(nec_state), nec_state->ip++, (UINT32)rc_b[rc_i++])')
    W.append('#undef FETCHWORD')
    W.append('#define FETCHWORD() (rc_lo = FETCH(), (UINT32)(rc_lo | (FETCH() << 8)))')
    W.append('#define fetchop(s) (prefetch(s), (s)->ip++, (UINT32)rc_b[rc_i++])')
    # V33 (chip_type 0) cycle constants
    W.append('#undef CLKS')
    W.append('#define CLKS(v20,v30,v33) { nec_state->icount-=(v33); }')
    W.append('#undef CLKW')
    W.append('#define CLKW(v20o,v30o,v33o,v20e,v30e,v33e,addr) { nec_state->icount-=((addr)&1)?(v33o):(v33e); }')
    W.append('#undef CLKM')
    W.append('#define CLKM(v20,v30,v33,v20m,v30m,v33m) { nec_state->icount-=(ModRM>=0xc0)?(v33):(v33m); }')
    W.append('#undef CLKR')
    W.append('#define CLKR(v20o,v30o,v33o,v20e,v30e,v33e,vall,addr) '
             '{ if (ModRM>=0xc0) nec_state->icount-=(vall); else nec_state->icount-=((addr)&1)?(v33o):(v33e); }')
    for name in ('necmodrm.h', 'necmacro.h'):
        for mname, text in parse_defines(name, SPECIAL_MACROS):
            if mname.startswith('CLK'):
                continue
            W.append('#undef ' + mname)
            W.append(text.replace(GETEA_CALL, 'RC_EA()').replace('table[nec_state->chip_type/8]', 'table[0]'))
    return W


def postamble():
    W = ['#undef Mod_RM', '#undef fetchop', '#undef RC_EA']
    W.append('#undef FETCH')
    W.append('#define FETCH() fetch(nec_state)')
    W.append('#undef FETCHWORD')
    W.append('#define FETCHWORD() fetchword(nec_state)')
    for name in ('necmodrm.h', 'necmacro.h'):
        for mname, text in parse_defines(name, SPECIAL_MACROS):
            W.append('#undef ' + mname)
            W.append(text)
    return W


RC_DO_PREFETCH = r'''static inline __attribute__((always_inline)) void rc_do_prefetch(nec_state_t *nec_state, int prev)
{
	// same as the closed form of do_prefetch for V33 (prefetch_cycles 1, size 6)
	int diff = prev - nec_state->icount;
	int k = -nec_state->prefetch_count;
	if (k > 0) {
		int m = (diff > 0) ? (diff - 1) : 0;
		if (m > k) m = k;
		diff -= m;
		nec_state->icount -= k - m;
		nec_state->prefetch_count = 0;
	}
	if (nec_state->prefetch_reset) { nec_state->prefetch_count = 0; nec_state->prefetch_reset = 0; return; }
	if (diff >= 1) { int room = 6 - nec_state->prefetch_count; if (room > 0) nec_state->prefetch_count += (diff < room) ? diff : room; }
}

#define RC_EXIT_CHECK() do { \
	if (nec_state->icount <= 0 || nec_state->stop_run || nec_state->no_interrupt) return; \
	if (nec_state->pending_irq && ((nec_state->pending_irq & NMI_IRQ) || nec_state->IF)) return; \
} while (0)
'''


def build_regions(pcs, max_insns, max_gap):
    regions = []
    cur = []
    last = None
    for pc in pcs:
        if cur and (len(cur) >= max_insns or pc - last > max_gap):
            regions.append(cur)
            cur = []
        cur.append(pc)
        last = pc
    if cur:
        regions.append(cur)
    return regions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True)
    ap.add_argument('--cover', action='append', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--exclude', action='append', default=['0x5a4'],
                    help='PCs not to compile (e.g. wait-loop skip PCs)')
    ap.add_argument('--max-insns', type=int, default=600)
    ap.add_argument('--max-gap', type=int, default=256)
    ap.add_argument('--spec', default='all', help='all | none | opcodes to specialize (e.g. 75,e2,02)')
    a = ap.parse_args()

    img = open(a.image, 'rb').read() + b'\0' * 16
    counts = collections.Counter()
    for path in a.cover:
        for line in open(path):
            p, c = line.split()
            counts[int(p, 16)] += int(c)
    exclude = {int(x, 0) for x in a.exclude}

    pcs = sorted(p for p in counts if p < IMAGE_END - 16 and p not in exclude)
    lens = {}
    bad = 0
    for pc in pcs:
        ln = insn_len_at(img, pc)
        if ln is None:
            bad += 1
            continue
        lens[pc] = ln
    pcs = [p for p in pcs if p in lens]
    pcset = set(pcs)

    # Self-check: fraction of fallthrough instructions whose next address is in coverage
    seq_ok = sum(1 for p in pcs if static_targets(img, p, lens[p])[1] and (p + lens[p]) in pcset)
    seq_all = sum(1 for p in pcs if static_targets(img, p, lens[p])[1])

    regions = build_regions(pcs, a.max_insns, a.max_gap)
    region_of = {p: ri for ri, r in enumerate(regions) for p in r}

    handlers = parse_handlers()
    ea = parse_ea()
    if a.spec == 'all':
        spec_ops = None
    elif a.spec == 'none':
        spec_ops = set()
    else:
        spec_ops = {int(x, 16) for x in a.spec.split(',')}
    nspec = 0

    out = []
    w = out.append
    w('// Auto-generated by tools/recomp/v33recomp.py - contains ROM-derived constants, do not commit')
    w('// %d instructions, %d regions' % (len(pcs), len(regions)))
    w('')
    w(RC_DO_PREFETCH)
    out.extend(preamble())
    w('')
    for ri, r in enumerate(regions):
        w('static void rei_rc_r%d(nec_state_t *nec_state, UINT32 pc)' % ri)
        w('{')
        w('\tswitch (pc) {')
        for p in r:
            w('\tcase 0x%05x: goto L_%05x;' % (p, p))
        w('\tdefault: return;')
        w('\t}')
        for p in r:
            ln = lens[p]
            tg, fall = static_targets(img, p, ln)
            j = p
            while img[j] in PREFIXES:
                j += 1
            body = None
            if spec_ops is None or img[j] in spec_ops:
                body = gen_spec(img, p, ln, handlers, ea)
            if body is None:
                body = gen_generic(img, p, ln)
            else:
                nspec += 1
            w('L_%05x: {' % p)
            w('\tint prev = nec_state->icount;')
            for line in body:
                w(line if line.startswith('#') else '\t' + line)
            w('\trc_do_prefetch(nec_state, prev);')
            w('\tRC_EXIT_CHECK();')
            local = [t for t in tg if t in pcset and region_of.get(t) == ri]
            if local:
                w('\tUINT32 n = PC(nec_state);')
                for t in local:
                    w('\tif (n == 0x%05x) goto L_%05x;' % (t, t))
            w('\treturn;')
            w('}')
        w('}')
        w('')
    out.extend(postamble())
    w('')
    w('static void (*const rei_rc_fns[%d])(nec_state_t *, UINT32) = {' % len(regions))
    for ri in range(len(regions)):
        w('\trei_rc_r%d,' % ri)
    w('};')
    w('static const UINT32 rei_rc_entries[%d] = {' % len(pcs))
    for p in pcs:
        w('\t0x%05x | (%du << 20),' % (p, region_of[p]))
    w('};')
    w('#define REI_RC_NUM_ENTRIES %d' % len(pcs))
    # At runtime, check the ROM matches the one used for generation (different set/dump: disable recomp, use interpreter)
    h = 0xcbf29ce484222325
    for byte in img[:IMAGE_END]:
        h = ((h ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    w('#define REI_RC_IMAGE_HASH 0x%016xULL' % h)
    w('#define REI_RC_NUM_REGIONS %d' % len(regions))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('recomp: instructions %d (specialized %d, unknown length %d), regions %d, fallthrough %d/%d' %
          (len(pcs), nspec, bad, len(regions), seq_ok, seq_all))


if __name__ == '__main__':
    sys.exit(main())
