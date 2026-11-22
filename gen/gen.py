#!/usr/bin/env python3
"""Generate one grammar's tables + lexer for tsasm from a tree-sitter generated parser.c.

Usage: gen.py path/to/parser.c <name> outdir

Emits outdir/lang_<name>.s (GNU as, intel syntax): parse tables, metadata, alias sequences, symbol
names, ts_lex / ts_lex_keywords translated into assembly state machines, and the language
descriptor. Its only global symbol is `const TsasmLanguage *tsasm_language_<name>(void)`; the
descriptor layout is the L_* offsets in src/parser.s.
"""
import re
import sys

src_path, NAME, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
assert re.fullmatch(r'[a-z][a-z0-9_]*', NAME), NAME
SRC = open(src_path).read()


def body_of(decl_regex):
    """Return text between the '{' after decl and its matching '};' at col 0."""
    m = re.search(decl_regex, SRC)
    assert m, decl_regex
    i = SRC.index('{', m.end() - 1)
    j = SRC.index('\n};', i)
    return SRC[i + 1:j]


def define(name):
    return int(re.search(r'#define %s (\d+)' % name, SRC).group(1))


STATE_COUNT = define('STATE_COUNT')
LARGE_STATE_COUNT = define('LARGE_STATE_COUNT')
SYMBOL_COUNT = define('SYMBOL_COUNT')
ALIAS_COUNT = define('ALIAS_COUNT')
TOKEN_COUNT = define('TOKEN_COUNT')
PRODUCTION_ID_COUNT = define('PRODUCTION_ID_COUNT')
MAX_ALIAS_SEQUENCE_LENGTH = define('MAX_ALIAS_SEQUENCE_LENGTH')
assert define('EXTERNAL_TOKEN_COUNT') == 0, "external scanners unsupported"
assert PRODUCTION_ID_COUNT <= 256, "nodes store the production id in one byte"
assert SYMBOL_COUNT + 5 < 0xFFFE

# ---- symbol ids -------------------------------------------------------------
sym = {'ts_builtin_sym_end': 0, 'ts_builtin_sym_error': 0xFFFF, 'ts_builtin_sym_error_repeat': 0xFFFE}
for name, val in re.findall(r'^\s+(\w+) = (\d+),', body_of(r'enum ts_symbol_identifiers \{'), re.M):
    sym[name] = int(val)
TOTAL_SYMS = SYMBOL_COUNT + ALIAS_COUNT


def num(tok):
    if isinstance(tok, int):
        return tok
    tok = tok.strip()
    if tok in sym:
        return sym[tok]
    m = re.fullmatch(r'(?:STATE|ACTIONS|SMALL_STATE)\((\d+)\)', tok)
    if m:
        v = int(m.group(1))
        return v - LARGE_STATE_COUNT if tok.startswith('SMALL_STATE') else v
    if tok in ('true',):
        return 1
    if tok in ('false',):
        return 0
    return int(tok, 0)


def designated(body):
    """Parse '[key] = value,' pairs (value without nested braces)."""
    out = {}
    for k, v in re.findall(r'\[([^\]]+)\]\s*=\s*([^,{}\n]+),', body):
        out[num(k)] = v.strip()
    return out


# ---- symbol names / map / metadata -----------------------------------------
names = {}
for k, v in re.findall(r'\[(\w+)\] = ("(?:[^"\\]|\\.)*"),', body_of(r'ts_symbol_names\[\] = \{')):
    names[num(k)] = v
public_map = {num(k): num(v) for k, v in designated(body_of(r'ts_symbol_map\[\] = \{')).items()}
meta = {}
for k, blk in re.findall(r'\[(\w+)\] = \{([^}]*)\}', body_of(r'ts_symbol_metadata\[\] = \{')):
    d = dict(re.findall(r'\.(\w+) = (\w+)', blk))
    meta[num(k)] = (d.get('visible') == 'true', d.get('named') == 'true')

# ---- alias sequences --------------------------------------------------------
alias_seq = [[0] * MAX_ALIAS_SEQUENCE_LENGTH for _ in range(PRODUCTION_ID_COUNT)]
for pid, blk in re.findall(r'\[(\d+)\] = \{([^}]*)\}', body_of(r'ts_alias_sequences\[PRODUCTION_ID_COUNT\]')):
    for i, a in re.findall(r'\[(\d+)\] = (\w+)', blk):
        alias_seq[int(pid)][int(i)] = num(a)

# ---- lex modes + reserved words -------------------------------------------
lex_state = [0] * STATE_COUNT
reserved_set = [0] * STATE_COUNT
for st, blk in re.findall(r'\[(\d+)\] = \{([^}]*)\}', body_of(r'ts_lex_modes\[STATE_COUNT\] = \{')):
    d = dict(re.findall(r'\.(\w+) = (\w+)', blk))
    lex_state[int(st)] = int(d['lex_state'])
    reserved_set[int(st)] = int(d.get('reserved_word_set_id', 0))
    assert 'external_lex_state' not in d
reserved = {}
if re.search(r'ts_reserved_words\[\d+\]\[MAX_RESERVED_WORD_SET_SIZE\] = \{', SRC):  # ABI 15 only
    rw_body = body_of(r'ts_reserved_words\[\d+\]\[MAX_RESERVED_WORD_SET_SIZE\] = \{')
    for sid, blk in re.findall(r'\[(\d+)\] = \{([^}]*)\}', rw_body):
        reserved[int(sid)] = [num(t) for t in blk.replace('\n', ' ').split(',') if t.strip()]
N_RESERVED_SETS = max(reserved) + 1 if reserved else 1

# ---- parse actions ----------------------------------------------------------
actions_body = body_of(r'ts_parse_actions\[\] = \{')
# entries are '[N] = {.entry = {.count = C, .reusable = R}}, ACT, ACT,'
entry_re = re.compile(r'\[(\d+)\] = \{\.entry = \{\.count = (\d+), \.reusable = (\w+)\}\},(.*)')
act_re = re.compile(r'(SHIFT_REPEAT|SHIFT_EXTRA|SHIFT|REDUCE|RECOVER|ACCEPT_INPUT)\(([^)]*)\)')
action_words = {}
max_action_index = 0
conflict_entries = 0
for line in actions_body.split('\n'):
    m = entry_re.search(line)
    if not m:
        continue
    idx, count = int(m.group(1)), int(m.group(2))
    acts = act_re.findall(m.group(4))
    assert len(acts) == count, line
    words = []
    for kind, args in acts:
        a = [x.strip() for x in args.split(',')] if args.strip() else []
        if kind == 'SHIFT_REPEAT':
            continue  # ignored by ts_parser__advance (action.shift.repetition -> break)
        if kind == 'SHIFT':
            words.append(0 | (num(a[0]) << 16))
        elif kind == 'SHIFT_EXTRA':
            words.append(0 | 4)
        elif kind == 'REDUCE':
            s, c, p, pid = num(a[0]), int(a[1]), int(a[2]), int(a[3])
            words.append(1 | (c << 8) | (s << 16) | (pid << 32) | ((p & 0xFFFF) << 48))
        elif kind == 'ACCEPT_INPUT':
            words.append(2)
        elif kind == 'RECOVER':
            words.append(3)
    if len(words) > 1:
        conflict_entries += 1
    # header word: low 8 bits = count of (non-repeat) actions.
    # bit 8: single-shift fast path flag
    action_words[idx] = [len(words) | ((m.group(3) == 'true') << 8)] + words
    max_action_index = max(max_action_index, idx + 1 + count)
actions = [0] * max_action_index
for idx, ws in action_words.items():
    for i, w in enumerate(ws):
        actions[idx + i] = w

# ---- parse table (dense) ----------------------------------------------------
table = [[0] * SYMBOL_COUNT for _ in range(STATE_COUNT)]
pt_body = body_of(r'ts_parse_table\[LARGE_STATE_COUNT\]\[SYMBOL_COUNT\] = \{')
large_rows = re.findall(r'\n  \[(?:STATE\()?(\d+)\)?\] = \{(.*?)\n  \}', pt_body, re.S)  # ABI 15: [STATE(n)], ABI 14: [n]
assert len(large_rows) == LARGE_STATE_COUNT, (len(large_rows), LARGE_STATE_COUNT)
for st, blk in large_rows:
    for k, v in re.findall(r'\[(\w+)\] = ((?:ACTIONS|STATE)\(\d+\))', blk):
        table[int(st)][num(k)] = num(v)
small_body = body_of(r'ts_small_parse_table\[\] = \{')
small_body = re.sub(r'\[\d+\] = ', '', small_body)
small = [num(t) for t in small_body.replace('\n', ' ').split(',') if t.strip()]
small_map = [int(v) for v in designated(body_of(r'ts_small_parse_table_map\[\] = \{')).values()]
assert len(small_map) == STATE_COUNT - LARGE_STATE_COUNT
for i, off in enumerate(small_map):
    st = LARGE_STATE_COUNT + i
    groups = small[off]
    p = off + 1
    for _ in range(groups):
        val, n = small[p], small[p + 1]
        for s in small[p + 2:p + 2 + n]:
            table[st][s] = val
        p += 2 + n

# ---- write tables.s ---------------------------------------------------------
out = []
w = out.append
w('# generated by gen.py from %s -- do not edit' % src_path)
w('\t.intel_syntax noprefix')
w('\t.section .rodata')


def emit_array(label, directive, values, per_line=16, align=64):
    w('\t.balign %d' % align)
    w('\t.globl %s' % label)
    w('%s:' % label)
    for i in range(0, len(values), per_line):
        w('\t%s %s' % (directive, ','.join(str(v) for v in values[i:i + per_line])))


flat = [v for row in table for v in row]
emit_array('tsg_parse_table', '.short', flat, 32)
emit_array('tsg_actions', '.quad', actions, 8)
emit_array('tsg_lex_state', '.short', lex_state)
emit_array('tsg_reserved_set', '.byte', reserved_set, 32)
# reserved word bitsets: per set, RWORDS qwords over token ids
RWORDS = (TOKEN_COUNT + 63) // 64
rw_bits = []
for s in range(N_RESERVED_SETS):
    bits = 0
    for t in reserved.get(s, []):
        bits |= 1 << t
    rw_bits += [(bits >> (64 * k)) & (2**64 - 1) for k in range(RWORDS)]
emit_array('tsg_reserved_bits', '.quad', rw_bits, 2)
# symbol info: byte flags: bit0 visible, bit1 named
info = []
for s in range(TOTAL_SYMS):
    v, n = meta.get(s, (False, False))
    info.append((1 if v else 0) | (2 if n else 0))
emit_array('tsg_symbol_flags', '.byte', info, 32)
emit_array('tsg_public_symbol', '.short', [public_map.get(s, s) for s in range(TOTAL_SYMS)])
# alias sequences padded to 16 entries per production for shift-indexing
ALIAS_STRIDE = 16
assert MAX_ALIAS_SEQUENCE_LENGTH <= ALIAS_STRIDE
emit_array('tsg_alias_seq', '.short', [v for row in alias_seq for v in row + [0] * (ALIAS_STRIDE - len(row))])
w('\t.section .data.rel.ro,"aw"')
w('\t.balign 8')
w('\t.globl tsg_symbol_names')
w('tsg_symbol_names:')
for s in range(TOTAL_SYMS):
    w('\t.quad tsg_name_%d' % s)
w('\t.section .rodata')
for s in range(TOTAL_SYMS):
    w('tsg_name_%d: .asciz %s' % (s, names.get(s, '""')))
TABLES = out

# word token for keyword extraction (ts_parser__lex); 0 when the grammar has none, like C
m = re.search(r'\.keyword_capture_token = (\w+)', SRC)
WORD = sym[m.group(1)] if m else 0
HAS_KW = 'static bool ts_lex_keywords(' in SRC

# ---- lexer translation ------------------------------------------------------
charsets = {}
for name, blk in re.findall(r'static (?:const )?TSCharacterRange (\w+)\[\] = \{(.*?)\n\};', SRC, re.S):
    rngs = []
    for a, b in re.findall(r"\{((?:'(?:[^'\\]|\\.)')|0x[0-9a-fA-F]+|\d+), ((?:'(?:[^'\\]|\\.)')|0x[0-9a-fA-F]+|\d+)\}", blk):
        rngs.append((None, a, b))
    charsets[name] = rngs

ESC = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, "'": 39, '"': 34, 'v': 11, 'f': 12, 'a': 7, 'b': 8}


def cval(tok):
    if tok.startswith("'"):
        body = tok[1:-1]
        if body.startswith('\\'):
            if body[1] == 'x':
                return int(body[2:], 16)
            if body[1] == 'u' or body[1] == 'U':
                return int(body[2:], 16)
            return ESC[body[1]]
        b = body.encode('utf-8').decode('utf-8')
        assert len(b) == 1, tok
        return ord(b)
    return int(tok, 0)


for name in charsets:
    charsets[name] = [(cval(a), cval(b)) for _, a, b in charsets[name]]

TOKRE = re.compile(r"\s*(\|\||&&|<=|>=|==|!=|<|>|\(|\)|,|!|'(?:[^'\\]|\\.|\\x[0-9a-fA-F]+)'|0x[0-9a-fA-F]+|\d+|\w+)")


def tokenize(s):
    toks, i = [], 0
    s = s.strip()
    while i < len(s):
        m = TOKRE.match(s, i)
        assert m and m.end() > i, (s, i)
        toks.append(m.group(1))
        i = m.end()
        while i < len(s) and s[i].isspace():
            i += 1
    return toks


class P:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def eat(self, x=None):
        tok = self.t[self.i]
        assert x is None or tok == x, (x, tok, self.t)
        self.i += 1
        return tok

    def orx(self):
        l = [self.andx()]
        while self.peek() == '||':
            self.eat()
            l.append(self.andx())
        return l[0] if len(l) == 1 else ('or', l)

    def andx(self):
        l = [self.prim()]
        while self.peek() == '&&':
            self.eat()
            l.append(self.prim())
        if len(l) == 2:  # range pattern  A <= la && la <= B
            a, b = l
            if a[0] == 'cmp' and b[0] == 'cmp' and a[1] == '>=' and b[1] == '<=':
                return ('range', a[2], b[2])
        return l[0] if len(l) == 1 else ('and', l)

    def prim(self):
        t = self.peek()
        if t == '(':
            self.eat()
            e = self.orx()
            self.eat(')')
            return e
        if t == '!':
            self.eat()
            return ('not', self.prim())
        if t == 'eof':
            self.eat()
            return ('eof',)
        if t == 'set_contains':
            self.eat()
            self.eat('(')
            name = self.eat()
            self.eat(',')
            self.eat()
            self.eat(',')
            self.eat('lookahead')
            self.eat(')')
            return ('set', name)
        # comparison: X op Y, one side is lookahead
        a = self.eat()
        op = self.eat()
        b = self.eat()
        if a == 'lookahead':
            return ('cmp', op, cval(b))
        assert b == 'lookahead', (a, op, b)
        flip = {'<': '>', '>': '<', '<=': '>=', '>=': '<=', '==': '==', '!=': '!='}
        return ('cmp', flip[op], cval(a))


def split_statements(case_body):
    """Split the body of a lexer case into statements."""
    stmts = []
    s = case_body.strip()
    i = 0
    while i < len(s):
        while i < len(s) and s[i].isspace():
            i += 1
        if i >= len(s):
            break
        if s.startswith('if (', i) or s.startswith('if(', i):
            # find matching paren
            j = s.index('(', i)
            depth = 0
            k = j
            while True:
                if s[k] == '(':
                    depth += 1
                elif s[k] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                elif s[k] == "'":
                    if s[k + 1] == '\\':
                        k = s.index("'", k + 3)
                    else:
                        k = k + 2
                    assert s[k] == "'"
                k += 1
            cond = s[j + 1:k]
            e = s.index(';', k)
            stmts.append(('if', cond, s[k + 1:e].strip()))
            i = e + 1
        elif s.startswith('ADVANCE_MAP(', i):
            e = s.index(');', i)
            stmts.append(('map', s[i + len('ADVANCE_MAP('):e]))
            i = e + 2
        else:
            e = s.index(';', i)
            stmts.append(('stmt', s[i:e].strip()))
            i = e + 1
    return stmts


def cases_of(fn_name):
    m = re.search(r'static bool %s\(TSLexer \*lexer, TSStateId state\) \{' % fn_name, SRC)
    i = m.end()
    j = SRC.index('\n}\n', i)
    body = SRC[i:j]
    parts = re.split(r'\n    case (\d+):\n', body)
    cases = {}
    for k in range(1, len(parts), 2):
        txt = parts[k + 1]
        txt = txt.split('\n    default:')[0]
        cases[int(parts[k])] = txt
    return cases


lex = []
L = lex.append
label_counter = [0]


def newlabel():
    label_counter[0] += 1
    return '.Lc%d' % label_counter[0]


used_sets = set()
adv_targets = set()
skip_targets = set()


def gen_cond(e, T, F, pre):
    """Emit code that jumps to T if e true, F if false. Falls through never."""
    kind = e[0]
    if kind == 'cmp':
        op, c = e[1], e[2]
        L('\tcmp eax, %d' % c)
        jt = {'==': 'je', '!=': 'jne', '<': 'jl', '<=': 'jle', '>': 'jg', '>=': 'jge'}[op]
        L('\t%s %s' % (jt, T))
        L('\tjmp %s' % F)
    elif kind == 'range':
        a, b = e[1], e[2]
        if a == b:
            L('\tcmp eax, %d' % a)
            L('\tje %s' % T)
        else:
            L('\tlea r8d, [rax-%d]' % a)
            L('\tcmp r8d, %d' % (b - a))
            L('\tjbe %s' % T)
        L('\tjmp %s' % F)
    elif kind == 'eof':
        L('\tcmp rsi, r9')
        L('\tjae %s' % T)
        L('\tjmp %s' % F)
    elif kind == 'set':
        used_sets.add(e[1])
        L('\tcmp eax, 127')
        slow = newlabel()
        L('\tja %s' % slow)
        L('\tlea r8, [rip + tsl_ascii_%s]' % e[1])
        L('\tcmp byte ptr [r8 + rax], 0')
        L('\tjne %s' % T)
        L('\tjmp %s' % F)
        L('%s:' % slow)
        L('\tlea r8, [rip + tsl_set_%s]' % e[1])
        L('\tmov edi, %d' % len(charsets[e[1]]))
        L('\tcall tsl_set_contains')
        L('\ttest r8d, r8d')
        L('\tjnz %s' % T)
        L('\tjmp %s' % F)
    elif kind == 'not':
        gen_cond(e[1], F, T, pre)
    elif kind == 'and':
        for sub in e[1][:-1]:
            nxt = newlabel()
            gen_cond(sub, nxt, F, pre)
            L('%s:' % nxt)
        gen_cond(e[1][-1], T, F, pre)
    elif kind == 'or':
        for sub in e[1][:-1]:
            nxt = newlabel()
            gen_cond(sub, T, nxt, pre)
            L('%s:' % nxt)
        gen_cond(e[1][-1], T, F, pre)
    else:
        raise ValueError(e)


def true_at_eof(e):
    """Value of a lexer condition at end of input (lookahead 0, eof true)."""
    k = e[0]
    if k == 'cmp':
        return {'==': 0 == e[2], '!=': 0 != e[2], '<': 0 < e[2], '<=': 0 <= e[2], '>': 0 > e[2], '>=': 0 >= e[2]}[e[1]]
    if k == 'range':
        return e[1] <= 0 <= e[2]
    if k == 'eof':
        return True
    if k == 'set':
        return any(a <= 0 <= b for a, b in charsets[e[1]])
    if k == 'not':
        return not true_at_eof(e[1])
    if k == 'and':
        return all(true_at_eof(x) for x in e[1])
    if k == 'or':
        return any(true_at_eof(x) for x in e[1])
    raise ValueError(e)


eof_edges = []  # (label, pre, from state, to state, target label) for transitions possible at eof


def eof_guarded(target, pre, st):
    """A transition taken at end of input advances by 0 bytes. If it lies on a cycle of such
    transitions the lexer would loop forever, so those edges get a check that ends the token;
    the others become plain aliases (resolved after translation)."""
    n = int(re.search(r'(\d+)$', target).group(1))
    label = '%s_eof%d' % (target, st)
    eof_edges.append((label, pre, st, n, target))
    return label


def action_target(act, pre):
    m = re.fullmatch(r'(ADVANCE|SKIP)\((\d+)\)', act)
    assert m, act
    n = int(m.group(2))
    if m.group(1) == 'ADVANCE':
        adv_targets.add((pre, n))
        return '%s_adv%d' % (pre, n)
    skip_targets.add((pre, n))
    return '%s_skip%d' % (pre, n)


jump_tables = []


def translate(fn_name, pre):
    cases = cases_of(fn_name)
    nstates = max(cases) + 1
    for st in range(nstates):
        txt = cases.get(st)
        # state entry: decode lookahead (eax=codepoint, edx=byte length; 0/0 at eof)
        L('%s_adv%d:' % (pre, st))
        L('\tadd rsi, rdx')
        L('%s_st%d:' % (pre, st))
        L('\tDECODE')
        if txt is None:
            L('\tjmp tsl_done')
            continue
        for stmt in split_statements(txt):
            if stmt[0] == 'if':
                cond = P(tokenize(stmt[1])).orx()
                T = action_target(stmt[2], pre)
                if true_at_eof(cond):
                    T = eof_guarded(T, pre, st)
                F = newlabel()
                gen_cond(cond, T, F, pre)
                L('%s:' % F)
            elif stmt[0] == 'map':
                vals = [cval(t) for t in tokenize(stmt[1]) if t != ',']
                pairs = list(zip(vals[0::2], vals[1::2]))
                for c, n in pairs:
                    adv_targets.add((pre, n))
                target = {c: ('%s_adv%d' % (pre, n) if c else eof_guarded('%s_adv%d' % (pre, n), pre, st)) for c, n in pairs}
                if len(pairs) >= 8 and all(c < 128 for c, _ in pairs):
                    tbl = 'tsl_jt%d' % len(jump_tables)
                    F = newlabel()
                    jump_tables.append((tbl, target, F))
                    L('\tcmp eax, 127')
                    L('\tja %s' % F)
                    L('\tlea rdi, [rip + %s]' % tbl)
                    L('\tmovsxd r8, dword ptr [rdi + rax*4]')
                    L('\tadd r8, rdi')
                    L('\tjmp r8')
                    L('%s:' % F)
                else:
                    for c, n in pairs:
                        L('\tcmp eax, %d' % c)
                        L('\tje %s' % target[c])
            else:
                s = stmt[1]
                if s.startswith('ACCEPT_TOKEN('):
                    symname = s[len('ACCEPT_TOKEN('):-1]
                    L('\tmov ecx, %d' % sym[symname])
                    L('\tmov r11, rsi')
                elif s == 'END_STATE()' or s == 'return false':
                    L('\tjmp tsl_done')
                elif s.startswith('eof = lexer->eof'):
                    pass
                else:
                    raise ValueError(s)
        L('\tjmp tsl_done')
    for (p, n) in sorted(skip_targets):
        if p != pre:
            continue
        L('%s_skip%d:' % (pre, n))
        L('\tadd rsi, rdx')
        L('\tmov r10, rsi')
        L('\tjmp %s_st%d' % (pre, n))
    # dispatch table
    L('\t.section .rodata')
    L('\t.balign 8')
    L('%s_dispatch:' % pre)
    for st in range(nstates):
        L('\t.long %s_st%d - %s_dispatch' % (pre, st, pre))
    L('\t.text')
    return nstates


L('# generated by gen.py -- do not edit')
L('\t.intel_syntax noprefix')
L('''
# Lexer register contract (internal, not ABI):
#   in : rsi = position to start lexing, r9 = end of input, ecx = lex state
#   out: ecx = result symbol (0xFFFFFFFF if no token), r10 = token start, r11 = token end
#   clobbers: rax, rdx, rdi, r8, rsi (=final lookahead position)
#   eax = lookahead codepoint (signed), edx = its byte length (0 at eof)
.macro DECODE
	xor eax, eax
	xor edx, edx
	cmp rsi, r9
	jae 1f
	movzx eax, byte ptr [rsi]
	inc edx
	test al, al
	jns 1f
	call tsl_decode_utf8
1:
.endm
	.text
	.p2align 4
tsl_lex_main:
	mov r10, rsi
	mov r11, rsi
	lea r8, [rip + tsl_main_dispatch]
	movsxd rax, dword ptr [r8 + rcx*4]
	add rax, r8
	mov ecx, -1
	xor edx, edx
	jmp rax
tsl_done:
	ret

# decode a multibyte utf-8 sequence at [rsi] (first byte already >= 0x80, in al)
# out: eax = codepoint or -1, edx = length (1 on error).  clobbers r8
tsl_decode_utf8:
	mov r8, r9
	sub r8, rsi             # bytes available
	cmp al, 0xC2
	jb .Lbad
	cmp al, 0xE0
	jae .L3
	# 2-byte
	cmp r8, 2
	jb .Lbad
	and eax, 0x1F
	movzx r8d, byte ptr [rsi+1]
	xor r8d, 0x80
	cmp r8d, 0x40
	jae .Lbad
	shl eax, 6
	or eax, r8d
	mov edx, 2
	ret
.L3:
	cmp al, 0xF0
	jae .L4
	cmp r8, 3
	jb .Lbad
	and eax, 0x0F
	movzx r8d, byte ptr [rsi+1]
	xor r8d, 0x80
	cmp r8d, 0x40
	jae .Lbad
	shl eax, 6
	or eax, r8d
	movzx r8d, byte ptr [rsi+2]
	xor r8d, 0x80
	cmp r8d, 0x40
	jae .Lbad
	shl eax, 6
	or eax, r8d
	cmp eax, 0x800
	jb .Lbad
	lea r8d, [rax-0xD800]
	cmp r8d, 0x800
	jb .Lbad
	mov edx, 3
	ret
.L4:
	cmp al, 0xF4
	ja .Lbad
	cmp r8, 4
	jb .Lbad
	and eax, 0x07
	movzx r8d, byte ptr [rsi+1]
	xor r8d, 0x80
	cmp r8d, 0x40
	jae .Lbad
	shl eax, 6
	or eax, r8d
	movzx r8d, byte ptr [rsi+2]
	xor r8d, 0x80
	cmp r8d, 0x40
	jae .Lbad
	shl eax, 6
	or eax, r8d
	movzx r8d, byte ptr [rsi+3]
	xor r8d, 0x80
	cmp r8d, 0x40
	jae .Lbad
	shl eax, 6
	or eax, r8d
	cmp eax, 0x10000
	jb .Lbad
	cmp eax, 0x10FFFF
	ja .Lbad
	mov edx, 4
	ret
.Lbad:
	mov eax, -1
	mov edx, 1
	ret

# binary search set_contains: r8 = ranges (pairs of int32), edi = count, eax = lookahead
# out: r8d = 1/0.  clobbers rdi; preserves ecx (the accepted symbol), rdx, rsi, rax
tsl_set_contains:
	push rdx
	push rbx
	push rcx
	mov ecx, edi
	xor edx, edx            # index
.Lsc_loop:
	cmp ecx, 1
	jbe .Lsc_last
	mov ebx, ecx
	shr ebx, 1              # half
	lea edi, [rdx+rbx]      # mid
	cmp eax, dword ptr [r8 + rdi*8]
	jl .Lsc_lt
	cmp eax, dword ptr [r8 + rdi*8 + 4]
	jle .Lsc_yes
	mov edx, edi
.Lsc_lt:
	sub ecx, ebx
	jmp .Lsc_loop
.Lsc_last:
	cmp eax, dword ptr [r8 + rdx*8]
	jl .Lsc_no
	cmp eax, dword ptr [r8 + rdx*8 + 4]
	jg .Lsc_no
.Lsc_yes:
	mov r8d, 1
	pop rcx
	pop rbx
	pop rdx
	ret
.Lsc_no:
	xor r8d, r8d
	pop rcx
	pop rbx
	pop rdx
	ret
''')
n_main = translate('ts_lex', 'tsl_main')
n_kw = 0
if HAS_KW:
    L('''	.p2align 4
tsl_lex_keyword:
	mov r10, rsi
	mov r11, rsi
	mov ecx, -1
	xor edx, edx
	jmp tsl_kw_st0''')
    n_kw = translate('ts_lex_keywords', 'tsl_kw')

L('\t.section .rodata')
for tbl, m, F in jump_tables:
    L('\t.balign 8')
    L('%s:' % tbl)
    for c in range(128):
        L('\t.long %s - %s' % (m.get(c, F), tbl))
for name in sorted(used_sets):
    rng = charsets[name]
    L('\t.balign 8')
    L('tsl_set_%s:' % name)
    for a, b in rng:
        L('\t.long %d, %d' % (a, b))
    asc = [0] * 128
    for a, b in rng:
        for c in range(a, min(b, 127) + 1):
            asc[c] = 1
    L('tsl_ascii_%s:' % name)
    L('\t.byte ' + ','.join(map(str, asc)))
L('\t.text')
graph = {}
for _, pre, a, b, _ in eof_edges:
    graph.setdefault((pre, a), set()).add((pre, b))


def reaches(x, y):
    seen, todo = set(), [x]
    while todo:
        v = todo.pop()
        if v == y:
            return True
        if v not in seen:
            seen.add(v)
            todo += graph.get(v, ())
    return False


eof_guards = 0
for label, pre, a, b, target in eof_edges:
    if reaches((pre, b), (pre, a)):
        eof_guards += 1
        L('%s:' % label)
        L('\ttest edx, edx')
        L('\tjz tsl_done')
        L('\tjmp %s' % target)
    else:
        L('\t.set %s, %s' % (label, target))

# ---- language descriptor (layout: L_* in src/parser.s) ------------------------
D = ['\t.section .data.rel.ro,"aw"', '\t.balign 64', 'tsg_language:']
D.append('\t.quad ' + ', '.join(['tsg_parse_table', 'tsg_actions', 'tsg_lex_state', 'tsg_reserved_set',
                                'tsg_reserved_bits', 'tsg_symbol_flags', 'tsg_public_symbol', 'tsg_alias_seq',
                                'tsg_symbol_names', 'tsl_lex_main', 'tsl_lex_keyword' if HAS_KW else '0']))
D.append('\t.long %d, %d, %d, %d, %d, 0' % (SYMBOL_COUNT, TOTAL_SYMS, WORD, RWORDS, TOKEN_COUNT))
D += ['\t.text', '\t.globl tsasm_language_%s' % NAME, '\t.type tsasm_language_%s, @function' % NAME,
      'tsasm_language_%s:' % NAME, '\tlea rax, [rip + tsg_language]', '\tret',
      '\t.size tsasm_language_%s, .-tsasm_language_%s' % (NAME, NAME),
      '\t.section .note.GNU-stack,"",@progbits']
text = '\n'.join(l for l in TABLES + lex + D if not l.startswith('\t.globl tsg_')
                 and not l.startswith('\t.section .note.GNU-stack'))
text = re.sub(r'\b(tsg|tsl)_', r'\1_%s_' % NAME, text) + '\n\t.section .note.GNU-stack,"",@progbits\n'
open('%s/lang_%s.s' % (outdir, NAME), 'w').write(text)

print('%s: states=%d large=%d symbols=%d tokens=%d actions=%d conflict_entries=%d lex_states=%d kw_states=%d eof_guards=%d' % (NAME,
    STATE_COUNT, LARGE_STATE_COUNT, SYMBOL_COUNT, TOKEN_COUNT, len(actions), conflict_entries, n_main, n_kw, eof_guards))
