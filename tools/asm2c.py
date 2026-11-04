#!/usr/bin/env python3
"""
asm2c.py - static recompiler for the JJFFE NASM sources.

Translates the 32-bit x86 assembly (ffe.asm + ffedat.asm + ffebmp.asm) into
portable C that runs on 64-bit hosts (Windows x64, Linux x86-64, ARM64...).

Model
-----
* All guest data lives in one contiguous "guest memory" block (g_mem).  Guest
  pointers are 32-bit offsets into that block, so the original 32-bit pointer
  arithmetic and data layout (dd tables of pointers, structs) keep working
  unchanged on a 64-bit host.
* Registers are C globals (eax ... edi, esp, ebp).  Flags are evaluated lazily.
  The x87 FPU is emulated with a small register stack of doubles.
* Code is split into "regions" (one per function, merged where functions jump
  into each other in loops).  Each region is one C function; jumps inside a
  region become goto, calls become C calls.  The guest stack is still used
  for arguments and a dummy return address is pushed, so [ebp+8] etc. work.
* Code labels whose address is taken (function pointers, jump tables) get a
  fake 32-bit "code address" (CODE_BASE + 4*n).  Indirect jumps/calls go
  through a dispatch table.
* External C functions (libc, platform API) become host thunks named X_<name>
  implemented in runtime/ffert.c.

Usage: asm2c.py <srcdir> <outdir>
"""
import os
import re
import struct
import sys
from collections import defaultdict, OrderedDict

DATA_BASE = 0x00010000
CODE_BASE = 0x7f000000
NFILES = 16

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp',
         'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SEGREGS = {'cs', 'ds', 'es', 'fs', 'gs', 'ss'}
SIZEKW = {'byte': 1, 'word': 2, 'dword': 4, 'qword': 8, 'tword': 10}

# Special labels in .text that are really data (code bytes used as floats).
TEXT_DATA = {
    'VideoData1': struct.pack('<I', 0x476a6000),   # 60000.0f
    'VideoData2': struct.pack('<I', 0x43fa0000),   # 500.0f
}


class Err(Exception):
    pass


def strip_comment(line):
    out = []
    q = None
    for ch in line:
        if q:
            out.append(ch)
            if ch == q:
                q = None
        elif ch in '\'"`':
            q = ch
            out.append(ch)
        elif ch == ';':
            break
        else:
            out.append(ch)
    return ''.join(out).rstrip()


def split_commas(s):
    parts, cur, q, depth = [], [], None, 0
    for ch in s:
        if q:
            cur.append(ch)
            if ch == q:
                q = None
            continue
        if ch in '\'"`':
            q = ch
        elif ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        elif ch == ',' and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    if cur or parts:
        parts.append(''.join(cur).strip())
    return parts


IDENT = r'[A-Za-z_.?$][A-Za-z0-9_.?$@#~]*'
DATA_DIRS = {'db': 1, 'dw': 2, 'dd': 4, 'dq': 8}
RES_DIRS = {'resb': 1, 'resw': 2, 'resd': 4, 'resq': 8}


class Item:
    __slots__ = ('kind', 'name', 'mn', 'ops', 'src', 'lineno', 'module',
                 'size', 'data', 'count', 'prefix')

    def __init__(self, kind, **kw):
        self.kind = kind
        self.name = self.mn = self.prefix = None
        self.ops = []
        self.src = ''
        self.lineno = 0
        self.module = ''
        self.size = 0
        self.data = None
        self.count = 1
        for k, v in kw.items():
            setattr(self, k, v)


class Module:
    def __init__(self, name):
        self.name = name
        self.globals = set()
        self.externs = set()
        self.text = []      # items in .text
        self.data = []      # items in .data/.bss (data/label)
        self.labelsec = {}  # qualified label -> 'text'|'data'


def read_lines(path, srcdir):
    res = []
    with open(path, 'r', encoding='latin-1') as f:
        for i, line in enumerate(f, 1):
            s = line.rstrip('\r\n')
            m = re.match(r'\s*%include\s+"([^"]+)"', s)
            if m:
                res.extend(read_lines(os.path.join(srcdir, m.group(1)), srcdir))
                continue
            res.append((os.path.basename(path), i, s))
    return res


def parse_module(name, path, srcdir):
    mod = Module(name)
    section = 'text'
    lastglobal = None
    for fname, lineno, raw in read_lines(path, srcdir):
        line = strip_comment(raw)
        if not line.strip():
            continue
        s = line.strip()
        toks = s.split(None, 1)
        kw = toks[0].upper()
        if kw == 'BITS':
            continue
        if kw == 'SECTION' or kw == 'SEGMENT':
            sec = toks[1].strip().lower()
            section = 'text' if sec.startswith('.text') else 'data'
            continue
        if kw == 'GLOBAL':
            mod.globals.add(toks[1].strip())
            continue
        if kw == 'EXTERN':
            mod.externs.add(toks[1].strip())
            continue
        # label?
        label = None
        if re.match(r'^\.[A-Za-z0-9_.]+$', s):
            s = s + ':'
        m = re.match(r'(' + IDENT + r')\s*:\s*(.*)$', s)
        if m:
            label, s = m.group(1), m.group(2).strip()
        else:
            # label without colon followed by data directive / times
            m = re.match(r'(' + IDENT + r')\s+(db|dw|dd|dq|resb|resw|resd|resq|times)\b(.*)$', s, re.I)
            if m and m.group(1).lower() not in ('rep', 'repne', 'repe', 'times'):
                label, s = m.group(1), (m.group(2) + m.group(3)).strip()
        if label is not None:
            if label.startswith('.'):
                if lastglobal is None:
                    raise Err('local label without parent %s:%d' % (fname, lineno))
                q = lastglobal + label
            else:
                q = label
                lastglobal = label
            it = Item('label', name=q, lineno=lineno, module=name, src=raw)
            if section == 'text' and q in TEXT_DATA:
                # treat as data: put it in data list
                mod.data.append(it)
                mod.data.append(Item('bytes', data=TEXT_DATA[q], module=name))
                mod.labelsec[q] = 'data'
                section_override_skip = True
                continue
            (mod.text if section == 'text' else mod.data).append(it)
            mod.labelsec[q] = section
            if not s:
                continue
        if lastglobal in TEXT_DATA and section == 'text':
            continue  # skip fake code bytes after TEXT_DATA label
        # directive or instruction
        t2 = s.split(None, 1)
        mn = t2[0].lower()
        rest = t2[1].strip() if len(t2) > 1 else ''
        count = 1
        if mn == 'times':
            m = re.match(r'(\S+)\s+(.*)$', rest)
            count = int(m.group(1), 0)
            t2 = m.group(2).split(None, 1)
            mn = t2[0].lower()
            rest = t2[1].strip() if len(t2) > 1 else ''
        if mn in DATA_DIRS or mn in RES_DIRS:
            it = Item('dir', mn=mn, ops=split_commas(rest) if mn in DATA_DIRS else [rest],
                      count=count, lineno=lineno, module=name, src=raw,
                      prefix=lastglobal)
            if section == 'text':
                # raw bytes inside code (deliberate crash in NullKill)
                it = Item('insn', mn='db_in_code', ops=[], lineno=lineno, module=name,
                          src=raw.strip(), prefix=None)
                it.name = lastglobal
                mod.text.append(it)
                continue
            mod.data.append(it)
            continue
        if section != 'text':
            raise Err('instruction in data %s:%d: %s' % (fname, lineno, raw))
        prefix = None
        if mn in ('rep', 'repe', 'repz', 'repne', 'repnz'):
            prefix = mn
            t2 = rest.split(None, 1)
            mn = t2[0].lower()
            rest = t2[1].strip() if len(t2) > 1 else ''
        it = Item('insn', mn=mn, ops=split_commas(rest) if rest else [],
                  lineno=lineno, module=name, src=raw.strip(), prefix=prefix)
        it.name = lastglobal  # scope for local labels
        mod.text.append(it)
    return mod


# --------------------------------------------------------------------------
# Expression handling

TOKRE = re.compile(r'\s*(0x[0-9a-fA-F]+|[0-9][0-9a-fA-F]*[hH]\b|[0-9]+|' + IDENT +
                   r"|'[^']*'|[-+*/()~|&^<>]+)")


def tokenize(expr):
    pos, out = 0, []
    expr = expr.strip()
    while pos < len(expr):
        m = TOKRE.match(expr, pos)
        if not m:
            raise Err('bad expression: %r' % expr)
        out.append(m.group(1))
        pos = m.end()
    return out


def parse_num(t):
    if t.lower().startswith('0x'):
        return int(t, 16)
    if t[-1] in 'hH':
        return int(t[:-1], 16)
    if t.startswith("'"):
        v = 0
        for i, ch in enumerate(t[1:-1]):
            v |= ord(ch) << (8 * i)
        return v
    return int(t, 10)


class Translator:
    def __init__(self, srcdir):
        self.srcdir = srcdir
        self.mods = []
        self.labels = {}       # global name -> address (data) / code addr
        self.labelkind = {}    # name -> 'data'|'code'|'extern'
        self.code_items = []   # flat list of (module, item)
        self.externs = OrderedDict()  # extern name -> code addr
        self.code_addr = {}    # code label -> fake address
        self.addr_taken = set()
        self.warnings = []

    # -- symbol naming: module-local labels in different modules may clash
    def qual(self, mod, name):
        return name

    def load(self):
        m1 = parse_module('ffe', os.path.join(self.srcdir, 'ffe.asm'), self.srcdir)
        m2 = parse_module('bmp', os.path.join(self.srcdir, 'ffebmp.asm'), self.srcdir)
        self.mods = [m1, m2]
        # check clashes
        n1 = set(m1.labelsec)
        n2 = set(m2.labelsec)
        clash = (n1 & n2)
        if clash:
            raise Err('label clashes between modules: %s' % sorted(clash)[:10])
        if m2.text:
            raise Err('ffebmp.asm has code')

    # -- data layout -----------------------------------------------------
    def data_size(self, it):
        if it.kind == 'bytes':
            return len(it.data)
        if it.mn in RES_DIRS:
            n = self.eval_const(it.ops[0], allow_syms=False)
            return RES_DIRS[it.mn] * n * it.count
        sz = DATA_DIRS[it.mn]
        total = 0
        for op in it.ops:
            op = op.strip()
            if op[0] in '\'"`':
                n = len(self.parse_string(op))
                if sz > 1:
                    n = (n + sz - 1) // sz * sz
                total += n
            else:
                total += sz
        return total * it.count

    def parse_string(self, s):
        q = s[0]
        assert s[-1] == q, s
        body = s[1:-1]
        if q == '`':
            body = body.encode('latin-1').decode('unicode_escape')
        return body.encode('latin-1')

    def layout(self):
        addr = DATA_BASE
        self.data_items = []
        for mod in self.mods:
            for it in mod.data:
                if it.kind == 'label':
                    self.labels[it.name] = addr
                    self.labelkind[it.name] = 'data'
                else:
                    it.size = self.data_size(it)
                    self.data_items.append((addr, it))
                    addr += it.size
        self.data_end = addr
        # code labels / externs get fake addresses
        n = 0
        for mod in self.mods:
            for it in mod.text:
                if it.kind == 'label':
                    self.code_addr[it.name] = CODE_BASE + 4 * n
                    self.labels[it.name] = CODE_BASE + 4 * n
                    self.labelkind[it.name] = 'code'
                    n += 1
        exts = set()
        for mod in self.mods:
            exts |= mod.externs
        for e in sorted(exts):
            if e in self.labels:
                continue  # defined in other module
            self.externs[e] = CODE_BASE + 4 * n
            self.labels[e] = CODE_BASE + 4 * n
            self.labelkind[e] = 'extern'
            n += 1
        self.ncode = n

    def resolve(self, name, scope):
        if name.startswith('.'):
            name = scope + name
        if name not in self.labels:
            raise Err('unknown symbol %s' % name)
        return name

    def eval_const(self, expr, allow_syms=True, scope=None, taken=None):
        toks = tokenize(expr)
        out = []
        for t in toks:
            if re.match(r'^(0x[0-9a-fA-F]+|[0-9][0-9a-fA-F]*[hH]|[0-9]+|\'.*\')$', t):
                out.append(str(parse_num(t)))
            elif re.match('^' + IDENT + '$', t):
                if t.lower() in SIZEKW or t.lower() in ('near', 'short', 'strict'):
                    continue
                if not allow_syms:
                    raise Err('symbol in const: %s' % expr)
                q = self.resolve(t, scope)
                if taken is not None and self.labelkind[q] != 'data':
                    taken.add(q)
                out.append(str(self.labels[q]))
            else:
                out.append(t.replace('/', '//'))
        return eval(' '.join(out), {}, {})

    def build_image(self):
        img = bytearray(self.data_end - DATA_BASE)
        for addr, it in self.data_items:
            off = addr - DATA_BASE
            if it.kind == 'bytes':
                img[off:off + len(it.data)] = it.data
                continue
            if it.mn in RES_DIRS:
                continue
            sz = DATA_DIRS[it.mn]
            chunk = bytearray()
            for op in it.ops:
                op = op.strip()
                if op[0] in '\'"`':
                    b = self.parse_string(op)
                    if sz > 1:
                        b = b + b'\0' * ((-len(b)) % sz)
                    chunk += b
                elif re.match(r'^-?[0-9]+\.[0-9]*(e[-+]?[0-9]+)?$', op):
                    chunk += struct.pack('<f' if sz == 4 else '<d', float(op))
                else:
                    v = self.eval_const(op, scope=it.prefix, taken=self.addr_taken)
                    chunk += (v & ((1 << (8 * sz)) - 1)).to_bytes(sz, 'little')
            chunk = chunk * it.count
            assert len(chunk) == it.size, (it.src, len(chunk), it.size)
            img[off:off + len(chunk)] = chunk
        self.image = img

    # -- code analysis ---------------------------------------------------
    def build_code(self):
        # flat list of code items in order
        self.code = []
        for mod in self.mods:
            self.code.extend(mod.text)
        # region starts
        self.label_index = {}
        for i, it in enumerate(self.code):
            if it.kind == 'label':
                self.label_index[it.name] = i
        call_targets = set()
        for it in self.code:
            if it.kind != 'insn':
                continue
            if it.mn == 'call' and len(it.ops) == 1:
                t = self.direct_target(it)
                if t and self.labelkind.get(t) == 'code':
                    call_targets.add(t)
            # address-taken code labels in operands
            if it.mn not in ('call', 'jmp') and not it.mn.startswith('j'):
                for op in it.ops:
                    for t in tokenize(op.replace('[', ' ').replace(']', ' ').replace(':', ' ')):
                        if re.match('^' + IDENT + '$', t):
                            tl = t.lower()
                            if tl in SIZEKW or tl in REG32 or tl in REG16 or tl in REG8L \
                                    or tl in REG8H or tl in SEGREGS or tl in ('near', 'short', 'st0', 'st1', 'st2', 'st3', 'st4', 'st5', 'st6', 'st7', 'st'):
                                continue
                            q = self.resolve(t, it.name)
                            if self.labelkind[q] != 'data':
                                self.addr_taken.add(q)
            elif it.mn in ('call', 'jmp'):
                # indirect through memory referencing code labels? only data
                pass
        starts = set()
        for mod in self.mods:
            for g in mod.globals:
                if self.labelkind.get(g) == 'code':
                    starts.add(g)
        starts |= call_targets
        for q in self.addr_taken:
            if self.labelkind.get(q) == 'code' and not q.startswith('JUMP_') and '.' not in q:
                starts.add(q)
        for it in self.code:
            if it.kind == 'label' and '.' not in it.name and \
                    (it.name.startswith('FUNC_') or it.name.startswith('_')):
                starts.add(it.name)
        # assign each item to a block (region id initially per start)
        region_of = [0] * len(self.code)
        rid = -1
        region_first = []
        for i, it in enumerate(self.code):
            if it.kind == 'label' and (it.name in starts or rid < 0):
                rid += 1
                region_first.append(i)
            region_of[i] = rid
        nreg = rid + 1
        self.label_region = {}
        for i, it in enumerate(self.code):
            if it.kind == 'label':
                self.label_region[it.name] = region_of[i]
        # cross-region jump edges (jmp/jcc and fallthrough)
        edges = defaultdict(set)
        for i, it in enumerate(self.code):
            r = region_of[i]
            if it.kind == 'insn' and (it.mn == 'jmp' or (it.mn.startswith('j') and it.mn != 'jmp')):
                t = self.direct_target(it)
                if t and self.labelkind.get(t) == 'code':
                    tr = self.label_region[t]
                    if tr != r:
                        edges[r].add(tr)
            if i + 1 < len(self.code) and region_of[i + 1] != r:
                # fallthrough possible?
                last = self.last_insn_before(i + 1)
                if last is not None and not self.is_terminal(last):
                    edges[r].add(region_of[i + 1])
        # SCC merge (Tarjan iterative)
        comp = self.scc(nreg, edges)
        # map region -> merged id
        self.region_of = [comp[r] for r in region_of]
        self.nregions = max(comp) + 1 if comp else 0
        merged = defaultdict(list)
        for r in range(nreg):
            merged[comp[r]].append(r)
        self.merged_count = sum(1 for v in merged.values() if len(v) > 1)
        # region names
        self.region_name = {}
        for i, it in enumerate(self.code):
            r = self.region_of[i]
            if r not in self.region_name and it.kind == 'label':
                self.region_name[r] = it.name
        # entries per region: labels that are reachable from outside region
        self.entries = defaultdict(list)   # region -> [label]
        self.entry_id = {}
        def add_entry(lbl):
            if lbl in self.entry_id:
                return
            r = self.region_of[self.label_index[lbl]]
            self.entry_id[lbl] = len(self.entries[r])
            self.entries[r].append(lbl)
        # first label of each region always entry 0
        for i, it in enumerate(self.code):
            if it.kind == 'label':
                r = self.region_of[i]
                if not self.entries[r]:
                    add_entry(it.name)
        for t in sorted(call_targets):
            add_entry(t)
        for t in sorted(self.addr_taken):
            if self.labelkind.get(t) == 'code':
                add_entry(t)
        for mod in self.mods:
            for g in sorted(mod.globals):
                if self.labelkind.get(g) == 'code':
                    add_entry(g)
        for i, it in enumerate(self.code):
            r = self.region_of[i]
            if it.kind == 'insn' and it.mn.startswith('j'):
                t = self.direct_target(it)
                if t and self.labelkind.get(t) == 'code' and self.region_of[self.label_index[t]] != r:
                    add_entry(t)
            if i + 1 < len(self.code) and self.region_of[i + 1] != r and self.code[i + 1].kind == 'label':
                add_entry(self.code[i + 1].name)

    def last_insn_before(self, idx):
        j = idx - 1
        while j >= 0 and self.code[j].kind != 'insn':
            j -= 1
        return self.code[j] if j >= 0 else None

    def is_terminal(self, it):
        return it.mn in ('jmp', 'ret', 'iret', 'retn')

    def scc(self, n, edges):
        index = [None] * n
        low = [0] * n
        onstack = [False] * n
        stack = []
        comp = [None] * n
        counter = [0]
        ncomp = [0]
        for v0 in range(n):
            if index[v0] is not None:
                continue
            work = [(v0, iter(sorted(edges.get(v0, ()))))]
            index[v0] = low[v0] = counter[0]; counter[0] += 1
            stack.append(v0); onstack[v0] = True
            while work:
                v, itr = work[-1]
                advanced = False
                for w in itr:
                    if index[w] is None:
                        index[w] = low[w] = counter[0]; counter[0] += 1
                        stack.append(w); onstack[w] = True
                        work.append((w, iter(sorted(edges.get(w, ())))))
                        advanced = True
                        break
                    elif onstack[w]:
                        low[v] = min(low[v], index[w])
                if advanced:
                    continue
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    while True:
                        w = stack.pop(); onstack[w] = False
                        comp[w] = ncomp[0]
                        if w == v:
                            break
                    ncomp[0] += 1
        # renumber in program order
        order = {}
        res = []
        for r in range(n):
            c = comp[r]
            if c not in order:
                order[c] = len(order)
            res.append(order[c])
        return res

    def direct_target(self, it):
        if len(it.ops) != 1:
            return None
        op = it.ops[0].strip()
        if '[' in op:
            return None
        toks = [t for t in tokenize(op) if t.lower() not in ('near', 'short', 'strict', 'dword')]
        if len(toks) == 1 and re.match('^' + IDENT + '$', toks[0]) and toks[0].lower() not in REG32:
            return self.resolve(toks[0], it.name)
        return None


# --------------------------------------------------------------------------
# C code generation for instructions

def cname(label):
    return 'L_' + re.sub(r'[^A-Za-z0-9_]', '_', label)


def rname(label):
    return 'R_' + re.sub(r'[^A-Za-z0-9_]', '_', label)


def sym(label):
    return 'A_' + re.sub(r'[^A-Za-z0-9_]', '_', label)


MASK = {1: '0xFFu', 2: '0xFFFFu', 4: '0xFFFFFFFFu'}
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}

CONDS = {
    'o': 'COND_O()', 'no': '!COND_O()',
    'c': 'COND_C()', 'b': 'COND_C()', 'nae': 'COND_C()',
    'nc': '!COND_C()', 'nb': '!COND_C()', 'ae': '!COND_C()',
    'z': 'COND_Z()', 'e': 'COND_Z()', 'nz': '!COND_Z()', 'ne': '!COND_Z()',
    'na': 'COND_BE()', 'be': 'COND_BE()', 'a': '!COND_BE()', 'nbe': '!COND_BE()',
    's': 'COND_S()', 'ns': '!COND_S()',
    'p': 'COND_P()', 'pe': 'COND_P()', 'np': '!COND_P()', 'po': '!COND_P()',
    'l': 'COND_L()', 'nge': 'COND_L()', 'nl': '!COND_L()', 'ge': '!COND_L()',
    'ng': 'COND_LE()', 'le': 'COND_LE()', 'g': '!COND_LE()', 'nle': '!COND_LE()',
}


class Op:
    """Parsed operand."""
    __slots__ = ('kind', 'size', 'reg', 'expr', 'text', 'sizehint', 'fpreg')

    def __init__(self):
        self.kind = None   # 'reg','mem','imm','st','seg'
        self.size = None
        self.reg = None
        self.expr = None
        self.text = ''
        self.sizehint = None
        self.fpreg = None


class Gen:
    def __init__(self, tr):
        self.tr = tr
        self.used_ext = set()

    def parse_op(self, text, scope):
        o = Op()
        o.text = text
        s = text.strip()
        # strip leading keywords
        while True:
            m = re.match(r'(byte|word|dword|qword|tword|near|short|strict|far)\b\s*(.*)$', s, re.I)
            if not m:
                break
            kw = m.group(1).lower()
            if kw in SIZEKW:
                o.sizehint = SIZEKW[kw]
            s = m.group(2).strip()
        sl = s.lower()
        if sl in REG32:
            o.kind, o.size, o.reg = 'reg', 4, sl
            return o
        if sl in REG16:
            o.kind, o.size, o.reg = 'reg', 2, sl
            return o
        if sl in REG8L or sl in REG8H:
            o.kind, o.size, o.reg = 'reg', 1, sl
            return o
        if sl in SEGREGS:
            o.kind, o.size, o.reg = 'seg', 2, sl
            return o
        m = re.match(r'^st\(?([0-7])?\)?$', sl)
        if m:
            o.kind = 'st'
            o.fpreg = int(m.group(1) or 0)
            return o
        if s.startswith('['):
            assert s.endswith(']'), text
            inner = s[1:-1].strip()
            m = re.match(r'^([cdefgs]s)\s*:\s*(.*)$', inner, re.I)
            if m:
                inner = m.group(2)
            o.kind = 'mem'
            o.size = o.sizehint
            o.expr = self.cexpr(inner, scope)
            return o
        o.kind = 'imm'
        o.size = o.sizehint
        o.expr = self.cexpr(s, scope)
        return o

    def cexpr(self, expr, scope):
        toks = tokenize(expr)
        out = []
        for t in toks:
            tl = t.lower()
            if re.match(r'^(0x[0-9a-fA-F]+|[0-9][0-9a-fA-F]*[hH]|[0-9]+|\'.*\')$', t):
                out.append('0x%Xu' % (parse_num(t) & 0xFFFFFFFF))
            elif tl in REG32:
                out.append(tl)
            elif re.match('^' + IDENT + '$', t):
                q = self.tr.resolve(t, scope)
                if self.tr.labelkind[q] == 'extern':
                    self.used_ext.add(q)
                out.append(sym(q))
            else:
                out.append(t)
        e = ' '.join(out)
        return '(uint32_t)(' + e + ')'

    # read/write helpers
    def rd(self, o, size=None):
        size = size or o.size
        if o.kind == 'reg':
            r = o.reg
            if r in REG32:
                return r
            if r in REG16:
                return '((uint16_t)%s)' % REG16[r]
            if r in REG8L:
                return '((uint8_t)%s)' % REG8L[r]
            return '((uint8_t)(%s >> 8))' % REG8H[r]
        if o.kind == 'mem':
            return 'RD%d(%s)' % (size * 8, o.expr)
        if o.kind == 'imm':
            if size == 4 or size is None:
                return o.expr
            return '((%s)%s)' % (UT[size], o.expr)
        if o.kind == 'seg':
            return '0u'
        raise Err('cannot read %s' % o.text)

    def wr(self, o, val, size=None):
        size = size or o.size
        if o.kind == 'reg':
            r = o.reg
            if r in REG32:
                return '%s = (uint32_t)(%s);' % (r, val)
            if r in REG16:
                R = REG16[r]
                return '%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);' % (R, R, val)
            if r in REG8L:
                R = REG8L[r]
                return '%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);' % (R, R, val)
            R = REG8H[r]
            return '%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);' % (R, R, val)
        if o.kind == 'mem':
            return 'WR%d(%s, %s);' % (size * 8, o.expr, val)
        if o.kind == 'seg':
            return '/* seg write ignored */'
        raise Err('cannot write %s' % o.text)

    def memtmp(self, o):
        """For read-modify-write on memory: hoist address to temp."""
        if o.kind == 'mem':
            n = Op()
            n.kind, n.size, n.expr, n.text = 'mem', o.size, '_a', o.text
            return 'uint32_t _a = %s; ' % o.expr, n
        return '', o

    def gen(self, it, ctx):
        """Return list of C lines for instruction."""
        mn = it.mn
        scope = it.name
        ops = [self.parse_op(t, scope) for t in it.ops]
        # infer sizes
        sizes = [o.size for o in ops if o.kind in ('reg',) and o.size]
        if mn not in ('movzx', 'movsx', 'shl', 'shr', 'sar', 'rcl', 'rol', 'ror', 'rcr', 'in', 'out'):
            dflt = sizes[0] if sizes else None
            for o in ops:
                if o.kind in ('mem', 'imm') and o.size is None:
                    o.size = dflt
        else:
            if ops and ops[0].kind == 'mem' and ops[0].size is None:
                ops[0].size = 4
        if mn.startswith('f') or mn in ('wait', 'sahf'):
            return self.gen_fpu(it, mn, ops)
        m = getattr(self, 'i_' + mn, None)
        if m is None:
            if mn.startswith('j') and mn[1:] in CONDS:
                return self.i_jcc(it, ops, CONDS[mn[1:]], ctx)
            if mn.startswith('set') and mn[3:] in CONDS:
                return [self.wr(ops[0], '(%s) ? 1 : 0' % CONDS[mn[3:]], 1)]
            raise Err('unsupported instruction %s (line %d): %s' % (mn, it.lineno, it.src))
        return m(it, ops, ctx)

    # ---- data movement
    def i_mov(self, it, ops, ctx):
        d, s = ops
        size = d.size or s.size or 4
        return [self.wr(d, self.rd(s, size), size)]

    def i_movzx(self, it, ops, ctx):
        d, s = ops
        if s.size is None:
            raise Err('movzx size: %s' % it.src)
        return [self.wr(d, '(uint32_t)(%s)%s' % (UT[s.size], self.rd(s)), d.size)]

    def i_movsx(self, it, ops, ctx):
        d, s = ops
        if s.size is None:
            raise Err('movsx size: %s' % it.src)
        return [self.wr(d, '(uint32_t)(int32_t)(%s)%s' % (ST[s.size], self.rd(s)), d.size)]

    def i_lea(self, it, ops, ctx):
        return [self.wr(ops[0], ops[1].expr, 4)]

    def i_xchg(self, it, ops, ctx):
        a, b = ops
        size = a.size or b.size
        pre, a2 = self.memtmp(a)
        if b.kind == 'mem':
            pre2, b2 = self.memtmp(b)
            pre += pre2.replace('_a', '_b')
            b2.expr = '_b'
        else:
            b2 = b
        return ['{ %suint32_t _t = %s; %s %s }' % (pre, self.rd(a2, size), self.wr(a2, self.rd(b2, size), size),
                                               self.wr(b2, '_t', size))]

    def i_push(self, it, ops, ctx):
        o = ops[0]
        if o.kind == 'imm':
            if o.sizehint == 2:
                return ['PUSH16(%s);' % o.expr]
            # byte imm is sign-extended (cexpr already gives 32-bit value)
            return ['PUSH32(%s);' % o.expr]
        if o.kind == 'seg':
            return ['PUSH32(0u);']
        size = o.size or 4
        if size == 2:
            return ['PUSH16(%s);' % self.rd(o)]
        if o.kind == 'reg' and o.reg == 'esp':
            return ['{ uint32_t _t = esp; PUSH32(_t); }']
        return ['{ uint32_t _t = %s; PUSH32(_t); }' % self.rd(o, 4)]

    def i_pop(self, it, ops, ctx):
        o = ops[0]
        if o.kind == 'seg':
            return ['esp += 4u;']
        size = o.size or 4
        if size == 2:
            return ['{ uint32_t _t = RD16(esp); esp += 2u; %s }' % self.wr(o, '_t', 2)]
        return ['{ uint32_t _t = RD32(esp); esp += 4u; %s }' % self.wr(o, '_t', 4)]

    def i_pushf(self, it, ops, ctx):
        return ['PUSH32(get_eflags());']

    # ---- arithmetic
    def arith(self, it, ops, op):
        d, s = ops
        size = d.size or s.size or 4
        pre, d2 = self.memtmp(d)
        a = self.rd(d2, size)
        b = self.rd(s, size)
        if op == 'add':
            body = 'uint32_t _x = %s, _y = %s; uint32_t _r = (_x + _y) & %s; SETF_ADD(_x, _y, _r, %d); %s' % (
                a, b, MASK[size], size, self.wr(d2, '_r', size))
        elif op == 'sub':
            body = 'uint32_t _x = %s, _y = %s; uint32_t _r = (_x - _y) & %s; SETF_SUB(_x, _y, _r, %d); %s' % (
                a, b, MASK[size], size, self.wr(d2, '_r', size))
        elif op == 'cmp':
            body = 'uint32_t _x = %s, _y = %s; uint32_t _r = (_x - _y) & %s; SETF_SUB(_x, _y, _r, %d);' % (
                a, b, MASK[size], size)
        elif op in ('and', 'or', 'xor'):
            c = {'and': '&', 'or': '|', 'xor': '^'}[op]
            if op == 'xor' and d.kind == 'reg' and s.kind == 'reg' and d.reg == s.reg:
                body = 'SETF_LOGIC(0u, %d); %s' % (size, self.wr(d2, '0u', size))
            else:
                body = 'uint32_t _r = (%s %s %s) & %s; SETF_LOGIC(_r, %d); %s' % (
                    a, c, b, MASK[size], size, self.wr(d2, '_r', size))
        elif op == 'test':
            body = 'uint32_t _r = (%s & %s) & %s; SETF_LOGIC(_r, %d);' % (a, b, MASK[size], size)
        elif op == 'adc':
            body = 'uint32_t _x = %s, _y = %s; uint32_t _c = COND_C(); uint32_t _r = (_x + _y + _c) & %s; SETF_ADC(_x, _y, _c, _r, %d); %s' % (
                a, b, MASK[size], size, self.wr(d2, '_r', size))
        elif op == 'sbb':
            body = 'uint32_t _x = %s, _y = %s; uint32_t _c = COND_C(); uint32_t _r = (_x - _y - _c) & %s; SETF_SBB(_x, _y, _c, _r, %d); %s' % (
                a, b, MASK[size], size, self.wr(d2, '_r', size))
        return ['{ %s%s }' % (pre, body)]

    def i_add(self, it, ops, ctx): return self.arith(it, ops, 'add')
    def i_sub(self, it, ops, ctx): return self.arith(it, ops, 'sub')
    def i_cmp(self, it, ops, ctx): return self.arith(it, ops, 'cmp')
    def i_and(self, it, ops, ctx): return self.arith(it, ops, 'and')
    def i_or(self, it, ops, ctx): return self.arith(it, ops, 'or')
    def i_xor(self, it, ops, ctx): return self.arith(it, ops, 'xor')
    def i_test(self, it, ops, ctx): return self.arith(it, ops, 'test')
    def i_adc(self, it, ops, ctx): return self.arith(it, ops, 'adc')
    def i_sbb(self, it, ops, ctx): return self.arith(it, ops, 'sbb')

    def i_inc(self, it, ops, ctx):
        d = ops[0]
        size = d.size or 4
        pre, d2 = self.memtmp(d)
        return ['{ %suint32_t _r = (%s + 1u) & %s; SETF_INC(_r, %d); %s }' % (
            pre, self.rd(d2, size), MASK[size], size, self.wr(d2, '_r', size))]

    def i_dec(self, it, ops, ctx):
        d = ops[0]
        size = d.size or 4
        pre, d2 = self.memtmp(d)
        return ['{ %suint32_t _r = (%s - 1u) & %s; SETF_DEC(_r, %d); %s }' % (
            pre, self.rd(d2, size), MASK[size], size, self.wr(d2, '_r', size))]

    def i_neg(self, it, ops, ctx):
        d = ops[0]
        size = d.size or 4
        pre, d2 = self.memtmp(d)
        return ['{ %suint32_t _y = %s; uint32_t _r = (0u - _y) & %s; SETF_SUB(0u, _y, _r, %d); %s }' % (
            pre, self.rd(d2, size), MASK[size], size, self.wr(d2, '_r', size))]

    def i_not(self, it, ops, ctx):
        d = ops[0]
        size = d.size or 4
        pre, d2 = self.memtmp(d)
        return ['{ %s%s }' % (pre, self.wr(d2, '~%s' % self.rd(d2, size), size))]

    def shift(self, it, ops, kind):
        d = ops[0]
        size = d.size or 4
        if len(ops) == 1:
            cnt = '1u'
        else:
            c = ops[1]
            cnt = self.rd(c, 1) if c.kind == 'reg' else c.expr
        pre, d2 = self.memtmp(d)
        a = self.rd(d2, size)
        return ['{ %suint32_t _n = (%s) & 31u; if (_n) { uint32_t _x = %s; uint32_t _r = SHIFT_%s(_x, _n, %d); %s } }' % (
            pre, cnt, a, kind, size, self.wr(d2, '_r', size))]

    def i_shl(self, it, ops, ctx): return self.shift(it, ops, 'SHL')
    def i_sal(self, it, ops, ctx): return self.shift(it, ops, 'SHL')
    def i_shr(self, it, ops, ctx): return self.shift(it, ops, 'SHR')
    def i_sar(self, it, ops, ctx): return self.shift(it, ops, 'SAR')
    def i_rcl(self, it, ops, ctx): return self.shift(it, ops, 'RCL')
    def i_rol(self, it, ops, ctx): return self.shift(it, ops, 'ROL')
    def i_ror(self, it, ops, ctx): return self.shift(it, ops, 'ROR')

    def i_imul(self, it, ops, ctx):
        if len(ops) == 1:
            s = ops[0]
            size = s.size or 4
            if size == 4:
                return ['{ int64_t _p = (int64_t)(int32_t)eax * (int64_t)(int32_t)%s; eax = (uint32_t)_p; edx = (uint32_t)((uint64_t)_p >> 32); SETF_MUL(_p != (int64_t)(int32_t)_p, eax, 4); }' % self.rd(s)]
            if size == 2:
                return ['{ int32_t _p = (int32_t)(int16_t)eax * (int32_t)(int16_t)%s; eax = (eax & 0xFFFF0000u) | ((uint32_t)_p & 0xFFFFu); edx = (edx & 0xFFFF0000u) | (((uint32_t)_p >> 16) & 0xFFFFu); SETF_MUL(_p != (int32_t)(int16_t)_p, (uint32_t)_p, 2); }' % self.rd(s)]
            return ['{ int32_t _p = (int32_t)(int8_t)eax * (int32_t)(int8_t)%s; eax = (eax & 0xFFFF0000u) | ((uint32_t)_p & 0xFFFFu); SETF_MUL(_p != (int32_t)(int8_t)_p, (uint32_t)_p, 1); }' % self.rd(s)]
        if len(ops) == 2:
            d, s = ops
            a, b = self.rd(d), self.rd(s, d.size)
        else:
            d, s, c = ops
            a, b = self.rd(s, d.size), self.rd(c, d.size)
        size = d.size
        if size != 4:
            raise Err('imul16 %s' % it.src)
        return ['{ int64_t _p = (int64_t)(int32_t)(%s) * (int64_t)(int32_t)(%s); %s SETF_MUL(_p != (int64_t)(int32_t)_p, (uint32_t)_p, 4); }' % (
            a, b, self.wr(d, '(uint32_t)_p', 4))]

    def i_mul(self, it, ops, ctx):
        s = ops[0]
        size = s.size or 4
        if size == 4:
            return ['{ uint64_t _p = (uint64_t)eax * (uint64_t)%s; eax = (uint32_t)_p; edx = (uint32_t)(_p >> 32); SETF_MUL(edx != 0, eax, 4); }' % self.rd(s)]
        if size == 2:
            return ['{ uint32_t _p = (uint32_t)(uint16_t)eax * (uint32_t)%s; eax = (eax & 0xFFFF0000u) | (_p & 0xFFFFu); edx = (edx & 0xFFFF0000u) | (_p >> 16); SETF_MUL((_p >> 16) != 0, _p, 2); }' % self.rd(s)]
        return ['{ uint32_t _p = (uint32_t)(uint8_t)eax * (uint32_t)%s; eax = (eax & 0xFFFF0000u) | (_p & 0xFFFFu); SETF_MUL((_p >> 8) != 0, _p, 1); }' % self.rd(s)]

    def i_div(self, it, ops, ctx):
        s = ops[0]
        size = s.size or 4
        if size == 4:
            return ['{ uint32_t _d = %s; uint64_t _n = ((uint64_t)edx << 32) | eax; if (!_d) guest_divide_error(); eax = (uint32_t)(_n / _d); edx = (uint32_t)(_n %% _d); }' % self.rd(s)]
        if size == 2:
            return ['{ uint32_t _d = %s; uint32_t _n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu); if (!_d) guest_divide_error(); eax = (eax & 0xFFFF0000u) | ((_n / _d) & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((_n %% _d) & 0xFFFFu); }' % self.rd(s)]
        return ['{ uint32_t _d = %s; uint32_t _n = eax & 0xFFFFu; if (!_d) guest_divide_error(); eax = (eax & 0xFFFF0000u) | ((_n / _d) & 0xFFu) | ((_n %% _d) << 8); }' % self.rd(s)]

    def i_idiv(self, it, ops, ctx):
        s = ops[0]
        size = s.size or 4
        if size == 4:
            return ['{ int32_t _d = (int32_t)%s; int64_t _n = (int64_t)(((uint64_t)edx << 32) | eax); if (!_d) guest_divide_error(); eax = (uint32_t)(int32_t)(_n / _d); edx = (uint32_t)(int32_t)(_n %% _d); }' % self.rd(s)]
        if size == 2:
            return ['{ int32_t _d = (int16_t)%s; int32_t _n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu)); if (!_d) guest_divide_error(); eax = (eax & 0xFFFF0000u) | ((uint32_t)(_n / _d) & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((uint32_t)(_n %% _d) & 0xFFFFu); }' % self.rd(s)]
        return ['{ int32_t _d = (int8_t)%s; int32_t _n = (int16_t)eax; if (!_d) guest_divide_error(); eax = (eax & 0xFFFF0000u) | ((uint32_t)(_n / _d) & 0xFFu) | (((uint32_t)(_n %% _d) & 0xFFu) << 8); }' % self.rd(s)]

    def i_cdq(self, it, ops, ctx):
        return ['edx = (eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;']

    def i_cwde(self, it, ops, ctx):
        return ['eax = (uint32_t)(int32_t)(int16_t)eax;']

    def i_nop(self, it, ops, ctx):
        return []

    def i_db_in_code(self, it, ops, ctx):
        return ['guest_fatal("executed data bytes in code (NullKill)");']

    def i_cli(self, it, ops, ctx):
        return ['/* cli */']

    def i_iret(self, it, ops, ctx):
        return ['guest_fatal("iret");', 'return;']

    # ---- string ops
    def i_movsb(self, it, ops, ctx): return self.strmov(it, 1)
    def i_movsw(self, it, ops, ctx): return self.strmov(it, 2)

    def i_movsd(self, it, ops, ctx):
        if ops:
            raise Err('sse movsd %s' % it.src)
        return self.strmov(it, 4)

    def strmov(self, it, size):
        if it.prefix in ('rep', 'repe', 'repz'):
            return ['REP_MOVS(%d);' % size]
        if it.prefix:
            raise Err('prefix %s' % it.src)
        return ['WR%d(edi, RD%d(esi)); esi += %du; edi += %du;' % (size * 8, size * 8, size, size)]

    def i_stosb(self, it, ops, ctx): return self.strsto(it, 1)
    def i_stosw(self, it, ops, ctx): return self.strsto(it, 2)
    def i_stosd(self, it, ops, ctx): return self.strsto(it, 4)

    def strsto(self, it, size):
        if it.prefix in ('rep', 'repe', 'repz'):
            return ['REP_STOS(%d);' % size]
        return ['WR%d(edi, eax); edi += %du;' % (size * 8, size)]

    def i_scasb(self, it, ops, ctx):
        if it.prefix in ('repne', 'repnz'):
            return ['REPNE_SCASB();']
        if it.prefix in ('repe', 'repz'):
            return ['REPE_SCASB();']
        return ['{ uint32_t _x = eax & 0xFFu, _y = RD8(edi); edi += 1u; SETF_SUB(_x, _y, (_x - _y) & 0xFFu, 1); }']

    # ---- control flow
    def jump_to(self, it, target, ctx):
        """C statement(s) transferring control to label target."""
        tr = self.tr
        if tr.labelkind[target] == 'extern':
            # tail jump to an external function
            self.used_ext.add(target)
            return 'X_%s(); return;' % target.lstrip('_')
        r = tr.region_of[tr.label_index[target]]
        if r == ctx['region']:
            return 'goto %s;' % cname(target)
        rn = rname(tr.region_name[r])
        return '%s(%d); return;' % (rn, tr.entry_id[target])

    def indirect(self, expr, ctx, is_call):
        # local switch for labels in this region whose address is taken
        if is_call:
            return 'PUSH32(RETADDR); guest_call(%s);' % expr
        cases = []
        for lbl in ctx['local_taken']:
            cases.append('case %s: goto %s;' % (sym(lbl), cname(lbl)))
        if cases:
            return '{ uint32_t _t = %s; switch (_t) { %s default: guest_call(_t); return; } }' % (expr, ' '.join(cases))
        return 'guest_call(%s); return;' % expr

    def i_jmp(self, it, ops, ctx):
        t = self.tr.direct_target(it)
        if t:
            if self.tr.labelkind[t] == 'data':
                raise Err('jump to data %s' % it.src)
            return [self.jump_to(it, t, ctx)]
        o = ops[0]
        return [self.indirect(self.rd(o, 4), ctx, False)]

    def i_jcc(self, it, ops, cond, ctx):
        t = self.tr.direct_target(it)
        return ['if (%s) { %s }' % (cond, self.jump_to(it, t, ctx))]

    def i_jecxz(self, it, ops, ctx):
        t = self.tr.direct_target(it)
        return ['if (ecx == 0) { %s }' % self.jump_to(it, t, ctx)]

    def i_loop(self, it, ops, ctx):
        t = self.tr.direct_target(it)
        return ['if (--ecx != 0) { %s }' % self.jump_to(it, t, ctx)]

    def i_call(self, it, ops, ctx):
        tr = self.tr
        t = tr.direct_target(it)
        if t == '_SetJmp':
            return ['GUEST_SETJMP();']
        if t == '_LongJmp':
            return ['PUSH32(RETADDR); guest_longjmp();']
        if t:
            if tr.labelkind[t] == 'extern':
                self.used_ext.add(t)
                return ['PUSH32(RETADDR); X_%s();' % t.lstrip('_')]
            r = tr.region_of[tr.label_index[t]]
            return ['PUSH32(RETADDR); %s(%d);' % (rname(tr.region_name[r]), tr.entry_id[t])]
        return [self.indirect(self.rd(ops[0], 4), ctx, True)]

    def i_ret(self, it, ops, ctx):
        if ops:
            return ['esp += 4u + %s; return;' % ops[0].expr]
        return ['esp += 4u; return;']

    i_retn = i_ret

    # ---- FPU
    def fmem(self, o, kind):
        # kind: 'f' float load, 'i' int load
        if kind == 'f':
            if o.size == 4:
                return 'RDF32(%s)' % o.expr
            if o.size == 8:
                return 'RDF64(%s)' % o.expr
            raise Err('fp size %s' % o.text)
        else:
            if o.size == 2:
                return '(double)(int16_t)RD16(%s)' % o.expr
            if o.size == 4:
                return '(double)(int32_t)RD32(%s)' % o.expr
            if o.size == 8:
                return '(double)(int64_t)RD64(%s)' % o.expr
            raise Err('fp int size %s' % o.text)

    def gen_fpu(self, it, mn, ops):
        if mn in ('wait', 'fwait'):
            return []
        if mn == 'sahf':
            return ['SAHF();']
        if mn == 'fld':
            o = ops[0]
            if o.kind == 'st':
                return ['{ double _v = ST(%d); FPUSH(_v); }' % o.fpreg]
            return ['FPUSH(%s);' % self.fmem(o, 'f')]
        if mn == 'fild':
            return ['FPUSH(%s);' % self.fmem(ops[0], 'i')]
        if mn == 'fldz':
            return ['FPUSH(0.0);']
        if mn == 'fld1':
            return ['FPUSH(1.0);']
        if mn in ('fst', 'fstp'):
            o = ops[0]
            pop = ' FPOP();' if mn == 'fstp' else ''
            if o.kind == 'st':
                return ['ST(%d) = ST(0);%s' % (o.fpreg, pop)]
            if o.size == 4:
                return ['WRF32(%s, ST(0));%s' % (o.expr, pop)]
            if o.size == 8:
                return ['WRF64(%s, ST(0));%s' % (o.expr, pop)]
            raise Err('fstp size %s' % it.src)
        if mn in ('fist', 'fistp'):
            o = ops[0]
            pop = ' FPOP();' if mn == 'fistp' else ''
            if o.size == 2:
                return ['WR16(%s, FIST16(ST(0)));%s' % (o.expr, pop)]
            if o.size == 4:
                return ['WR32(%s, FIST32(ST(0)));%s' % (o.expr, pop)]
            if o.size == 8:
                return ['WR64(%s, FIST64(ST(0)));%s' % (o.expr, pop)]
            raise Err('fistp size %s' % it.src)
        arith = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/',
                 'fsubr': 'r-', 'fdivr': 'r/'}
        base = mn[:-1] if mn.endswith('p') and mn[:-1] in arith else mn
        if base in arith:
            op = arith[base]
            rev = op.startswith('r')
            op = op[-1]
            pop = mn.endswith('p') and base != mn
            if len(ops) == 0:
                # fxxxp  => st1 = st1 op st0, pop
                dst, src = 1, 0
                return ['ST(%d) = %s; FPOP();' % (dst, self.fexpr('ST(%d)' % dst, 'ST(%d)' % src, op, rev))]
            if len(ops) == 1 and ops[0].kind != 'st':
                v = self.fmem(ops[0], 'f')
                return ['ST(0) = %s;' % self.fexpr('ST(0)', v, op, rev)]
            if len(ops) == 1:
                d, s = 0, ops[0].fpreg
            else:
                d, s = ops[0].fpreg, ops[1].fpreg
            line = 'ST(%d) = %s;' % (d, self.fexpr('ST(%d)' % d, 'ST(%d)' % s, op, rev))
            if pop:
                line += ' FPOP();'
            return [line]
        if mn in ('fiadd', 'fisub', 'fimul', 'fidiv', 'fisubr', 'fidivr'):
            b = 'f' + mn[2:]
            op = arith[b]
            rev = op.startswith('r')
            return ['ST(0) = %s;' % self.fexpr('ST(0)', self.fmem(ops[0], 'i'), op[-1], rev)]
        if mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp'):
            if mn.startswith('fi'):
                v = self.fmem(ops[0], 'i')
            elif not ops:
                v = 'ST(1)'
            elif ops[0].kind == 'st':
                v = 'ST(%d)' % ops[0].fpreg
            else:
                v = self.fmem(ops[0], 'f')
            npop = 2 if mn.endswith('pp') else (1 if mn.endswith('p') else 0)
            return ['FCOM(ST(0), %s);%s' % (v, ' FPOP();' * npop)]
        if mn == 'ftst':
            return ['FCOM(ST(0), 0.0);']
        if mn in ('fnstsw', 'fstsw'):
            return [self.wr(ops[0], 'FSTSW()', 2)]
        if mn in ('fnstcw', 'fstcw'):
            return [self.wr(ops[0], 'fpu_cw', 2)]
        if mn == 'fldcw':
            return ['fpu_cw = (uint16_t)%s;' % self.rd(ops[0], 2)]
        if mn == 'fprem':
            return ['FPREM();']
        if mn == 'fchs':
            return ['ST(0) = -ST(0);']
        if mn == 'fabs':
            return ['ST(0) = fabs(ST(0));']
        if mn == 'fsqrt':
            return ['ST(0) = sqrt(ST(0));']
        if mn == 'fxch':
            r = ops[0].fpreg if ops else 1
            return ['{ double _t = ST(0); ST(0) = ST(%d); ST(%d) = _t; }' % (r, r)]
        if mn in ('fninit', 'finit'):
            return ['FINIT();']
        raise Err('unsupported fpu %s: %s' % (mn, it.src))

    def fexpr(self, a, b, op, rev):
        if rev:
            a, b = b, a
        return '(%s %s %s)' % (a, op, b)


# --------------------------------------------------------------------------

def main():
    srcdir, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    tr = Translator(srcdir)
    tr.load()
    tr.layout()
    tr.build_image()
    tr.build_code()
    gen = Gen(tr)

    # group code items by region
    reg_items = defaultdict(list)
    for i, it in enumerate(tr.code):
        reg_items[tr.region_of[i]].append(i)

    # per-region local address-taken labels
    taken_by_region = defaultdict(list)
    for lbl in sorted(tr.addr_taken):
        if tr.labelkind.get(lbl) == 'code':
            taken_by_region[tr.region_of[tr.label_index[lbl]]].append(lbl)

    # ---- header with symbols
    with open(os.path.join(outdir, 'ffe_syms.h'), 'w') as f:
        f.write('/* Generated by tools/asm2c.py - do not edit */\n')
        f.write('#ifndef FFE_SYMS_H\n#define FFE_SYMS_H\n')
        f.write('#define GUEST_DATA_BASE 0x%08Xu\n' % DATA_BASE)
        f.write('#define GUEST_DATA_END 0x%08Xu\n' % tr.data_end)
        f.write('#define GUEST_CODE_BASE 0x%08Xu\n' % CODE_BASE)
        f.write('#define GUEST_CODE_COUNT %du\n' % tr.ncode)
        for name, addr in sorted(tr.labels.items(), key=lambda x: x[1]):
            f.write('#define %s 0x%08Xu\n' % (sym(name), addr))
        f.write('\n')
        for r in range(tr.nregions):
            f.write('void %s(int e);\n' % rname(tr.region_name[r]))
        f.write('#endif\n')

    # ---- data image
    img = tr.image
    with open(os.path.join(outdir, 'ffe_data.c'), 'w') as f:
        f.write('/* Generated by tools/asm2c.py - initial guest data image */\n')
        f.write('#include <stdint.h>\n')
        f.write('const uint32_t ffe_data_size = %du;\n' % len(img))
        pad = img + b'\0' * ((-len(img)) % 4)
        words = struct.unpack('<%dI' % (len(pad) // 4), pad)
        f.write('const uint32_t ffe_data_image[%d] = {\n' % len(words))
        for k in range(0, len(words), 8):
            f.write(','.join('0x%x' % w for w in words[k:k + 8]) + ',\n')
        f.write('};\n')

    # ---- code files
    regions = list(range(tr.nregions))
    per = (len(regions) + NFILES - 1) // NFILES
    errors = []
    for fi in range(NFILES):
        rs = regions[fi * per:(fi + 1) * per]
        with open(os.path.join(outdir, 'ffe_code%02d.c' % fi), 'w') as f:
            f.write('/* Generated by tools/asm2c.py from ffe.asm - do not edit */\n')
            f.write('#include "ffert.h"\n#include "ffe_syms.h"\n\n')
            for r in rs:
                name = tr.region_name[r]
                ctx = {'region': r, 'local_taken': taken_by_region[r]}
                f.write('void %s(int e)\n{\n' % rname(name))
                ents = tr.entries[r]
                if len(ents) > 1:
                    f.write('\tswitch (e) {\n')
                    for k, lbl in enumerate(ents):
                        f.write('\tcase %d: goto %s;\n' % (k, cname(lbl)))
                    f.write('\t}\n')
                else:
                    f.write('\t(void)e;\n')
                idxs = reg_items[r]
                prev = None
                for i in idxs:
                    # non-contiguous (merged SCC) segments: explicit jump at segment end
                    if prev is not None and i != prev + 1:
                        last = tr.code[prev]
                        if last.kind != 'insn' or not tr.is_terminal(last):
                            # fallthrough into next code item (which is in another position)
                            nxt = prev + 1
                            while nxt < len(tr.code) and tr.code[nxt].kind != 'label':
                                nxt += 1
                            if nxt < len(tr.code):
                                f.write('\t%s\n' % gen.jump_to(None, tr.code[nxt].name, ctx))
                    it = tr.code[i]
                    if it.kind == 'label':
                        f.write('%s: ;\n' % cname(it.name))
                    else:
                        try:
                            lines = gen.gen(it, ctx)
                        except Err as ex:
                            errors.append(str(ex))
                            lines = ['guest_fatal("untranslated: %s");' % it.src.replace('\\', '\\\\').replace('"', '\\"')]
                        f.write('\t/* %s */\n' % it.src.replace('*/', '* /'))
                        for ln in lines:
                            f.write('\t%s\n' % ln)
                    prev = i
                # end of region: fall into the next region if needed
                last_i = idxs[-1]
                last = tr.code[last_i]
                if last.kind != 'insn' or not tr.is_terminal(last):
                    nxt = last_i + 1
                    if nxt < len(tr.code):
                        f.write('\t%s\n' % gen.jump_to(None, tr.code[nxt].name, ctx))
                    else:
                        f.write('\tguest_fatal("fell off end of code");\n')
                f.write('}\n\n')

    # ---- dispatch table + extern list
    with open(os.path.join(outdir, 'ffe_dispatch.c'), 'w') as f:
        f.write('/* Generated by tools/asm2c.py - code address dispatch */\n')
        f.write('#include "ffert.h"\n#include "ffe_syms.h"\n\n')
        exts = sorted(tr.externs)
        for e in exts:
            f.write('void X_%s(void);\n' % e.lstrip('_'))
        f.write('\nconst GuestCodeEntry ffe_code_table[%d] = {\n' % tr.ncode)
        byaddr = {}
        for lbl, addr in tr.code_addr.items():
            byaddr[addr] = lbl
        for k in range(tr.ncode):
            addr = CODE_BASE + 4 * k
            if addr in byaddr:
                lbl = byaddr[addr]
                if lbl in tr.entry_id:
                    r = tr.region_of[tr.label_index[lbl]]
                    f.write('\t{ %s, %d, 0, "%s" },\n' % (rname(tr.region_name[r]), tr.entry_id[lbl], lbl))
                else:
                    f.write('\t{ 0, 0, 0, "%s" },\n' % lbl)
            else:
                e = [x for x, a in tr.externs.items() if a == addr][0]
                f.write('\t{ 0, 0, X_%s, "%s" },\n' % (e.lstrip('_'), e))
        f.write('};\n')

    print('regions: %d (merged SCCs: %d), code labels+externs: %d, data: %d bytes' % (
        tr.nregions, tr.merged_count, tr.ncode, len(img)))
    print('externs:', ' '.join(sorted(tr.externs)))
    if errors:
        print('%d untranslated instructions:' % len(errors))
        seen = set()
        for e in errors:
            if e not in seen:
                print('  ', e)
                seen.add(e)
        sys.exit(1)


if __name__ == '__main__':
    main()
