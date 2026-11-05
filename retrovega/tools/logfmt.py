#!/usr/bin/env python3
"""The log's formats out of the DOS build's memory (the user, 2026-11-04: the memory plan's "log formats into an XMS
string table"): tools/logfmt.py OUTDIR TABLE SRC.c ...; tools/logfmt.py --append EXE TABLE

Every format the log alone reads is a string in the program's far data, some 12 KB of conventional memory.  This writes
a copy of each source into OUTDIR with each such format's text replaced by a token, "\\001" and its number, and the
texts in order into TABLE, which build.sh appends to the EXE (past its load image: DOS never loads it).  At the start
the game copies the table into XMS (util.c logfmt_load), and rv_logf / rv_notef put a token's text back before they
print, so the log reads as before.

What is a format the log alone reads:
 - the first argument of RV_LOG( or RV_NOTE( (a literal, perhaps in parts on several lines);
 - a `static const char __far NAME[] = "..." ...;` (one of a list, `..., __far NAME2[] = "...";`) whose every other
   mention in its file is as the first argument of rv_logf( or rv_notef(.
Anything else is left as it is.  Lines stay where they were (a literal in parts keeps its line breaks), so the compiler's
line numbers are the source's.

TABLE: "RVLOGFMT", a u16 count, count u32 offsets from the table's start, the texts NUL-terminated; then the EXE's
trailer, written by build.sh: the table's length (u32) and "RVLOGEND".
"""
import os
import re
import struct
import sys

STR = r'"(?:[^"\\\n]|\\.)*"'
LITS = re.compile(r'(?:%s\s*)+' % STR)          # adjacent literals, with what is between them


def c_unescape(lit):
    """the bytes of a C string literal's body (the escapes the sources use)"""
    out, i = bytearray(), 0
    while i < len(lit):
        c = lit[i]
        if c != '\\':
            out += c.encode('latin-1')
            i += 1
            continue
        n = lit[i + 1]
        simple = {'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34, "'": 39, '0': 0, 'a': 7, 'b': 8, 'f': 12, 'v': 11, '?': 63}
        if n == 'x':
            m = re.match(r'[0-9a-fA-F]+', lit[i + 2:])
            out.append(int(m.group(0), 16) & 255)
            i += 2 + len(m.group(0))
        elif n in '01234567':
            m = re.match(r'[0-7]{1,3}', lit[i + 1:])
            out.append(int(m.group(0), 8) & 255)
            i += 1 + len(m.group(0))
        else:
            out.append(simple[n])
            i += 2
    return bytes(out)


def literal_text(span):
    """the text of adjacent literals (with whatever lies between them)"""
    return b''.join(c_unescape(m[1:-1]) for m in re.findall(STR, span))


def token(n, span):
    """the literals' place: the token, then empty literals on the lines they had, so the lines stay"""
    return '"\\001" "%d"' % n + '\n'.join([''] + ['""'] * span.count('\n')) if '\n' in span else '"\\001" "%d"' % n


def strip_comments(src):
    """the source with its comments blanked (lengths and lines kept), for the searching only"""
    out = []
    i = 0
    while i < len(src):
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = len(src) if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
        elif src.startswith('//', i):
            j = src.find('\n', i)
            j = len(src) if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif src[i] == '"':
            m = re.match(STR, src[i:])
            out.append(m.group(0))
            i += len(m.group(0))
        elif src[i] == "'":
            m = re.match(r"'(?:[^'\\\n]|\\.)*'", src[i:])
            out.append(m.group(0))
            i += len(m.group(0))
        else:
            out.append(src[i])
            i += 1
    return ''.join(out)


def literal_end(code, i):
    """the end of the literal starting at code[i] (a '"')"""
    m = re.match(STR, code[i:])
    return i + len(m.group(0))


def transform(src, table):
    code = strip_comments(src)
    edits = []                                   # (start, end) of the literals to put a token in place of
    # RV_LOG( / RV_NOTE( literals
    for m in re.finditer(r'\bRV_(?:LOG|NOTE)\s*\(\s*', code):
        lm = LITS.match(code, m.end())
        if lm:
            edits.append((lm.start(), lm.start() + len(code[lm.start():lm.end()].rstrip())))
    # far formats read only by rv_logf / rv_notef: each declarator of a `static const char __far` statement
    for m in re.finditer(r'\bstatic\s+const\s+char\s+__far\b', code):
        i = j = m.end()
        while j < len(code) and code[j] != ';':
            j = literal_end(code, j) if code[j] == '"' else j + 1
        k = i
        while k < j:
            d = re.compile(r'\s*(?:__far\s+)?(\w+)\s*\[\s*\]\s*=\s*').match(code, k)
            if not d:
                break
            lm = LITS.match(code, d.end())
            if not lm:
                break
            end = lm.start() + len(code[lm.start():lm.end()].rstrip())
            name, k = d.group(1), end
            while k < j and code[k] in ' \t\n,':
                k += 1
            uses = [u.start() for u in re.finditer(r'\b%s\b' % re.escape(name), code) if u.start() != d.start(1)]
            if uses and all(re.search(r'\b(?:rv_logf|rv_notef)\s*\(\s*$', code[max(0, u - 40):u]) for u in uses):
                edits.append((lm.start(), end))
    edits.sort()
    out, at = [], 0
    for s_, e in edits:
        out.append(src[at:s_])
        span = src[s_:e]
        table.append(literal_text(span))
        out.append(token(len(table) - 1, span))
        at = e
    out.append(src[at:])
    return ''.join(out), len(edits)


def append(exe, table_path):
    """the table onto the EXE, past its load image, and its trailer: the length (u32) and "RVLOGEND" """
    blob = open(table_path, 'rb').read()
    with open(exe, 'ab') as f:
        f.write(blob + struct.pack('<I', len(blob)) + b'RVLOGEND')


def main():
    if sys.argv[1] == '--append':
        append(sys.argv[2], sys.argv[3])
        return
    outdir, table_path, srcs = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(outdir, exist_ok=True)
    table, saved = [], 0
    for path in srcs:
        src = open(path, encoding='latin-1').read()
        out, n = transform(src, table)
        open(os.path.join(outdir, os.path.basename(path)), 'w', encoding='latin-1').write(out)
    blob = bytearray(b'RVLOGFMT' + struct.pack('<H', len(table)))
    at = len(blob) + 4 * len(table)
    offs = []
    for t in table:
        offs.append(at)
        at += len(t) + 1
    blob += b''.join(struct.pack('<I', o) for o in offs)
    for t in table:
        blob += t + b'\0'
    open(table_path, 'wb').write(blob)
    texts = sum(len(t) + 1 for t in table)
    tokens = sum(len(b'\001%d' % i) + 1 for i in range(len(table)))
    print('logfmt: %d formats, %d bytes of text out of memory (%d bytes of tokens in its place), table %d bytes'
          % (len(table), texts, tokens, len(blob)))


main()
