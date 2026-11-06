#!/usr/bin/env python3
"""The log's formats out of the DOS build's memory (the user, 2026-11-04: the memory plan's "log formats into an XMS
string table"): tools/logfmt.py OUTDIR TABLE SRC.c ... [--then TABLE2 SRC2.c ...]; tools/logfmt.py --append EXE TABLE

Every format the log alone reads is a string in the program's far data, some 12 KB of conventional memory.  This writes
a copy of each source into OUTDIR with each such format's text replaced by a token, "\\001" and its number, and the
texts in order into TABLE, which build.sh appends to the EXE (past its load image: DOS never loads it).  At the start
the game copies the table into XMS (util.c logfmt_load), and rv_logf / rv_notef put a token's text back before they
print, so the log reads as before.

What is a format the log alone reads:
 - the first argument of RV_LOG( or RV_NOTE( (a literal, perhaps in parts on several lines);
 - a `static const char __far NAME[] = "..." ...;` (one of a list, `..., __far NAME2[] = "...";`) whose every other
   mention in its file is as the first argument of rv_logf( or rv_notef(, or the whole of a play test's message
   argument (tests/play/playtest.c: the second of REQUIRE( and EXPECT(, the first of pt_fail( and ln_f(, whose ln_f
   puts a token's text back; 2026-11-06: RVP.EXE's 48 KB of messages had left its flights no room for the shot list).
Anything else is left as it is.  Lines stay where they were (a literal in parts keeps its line breaks), so the compiler's
line numbers are the source's.

--then: SRC2's formats numbered after SRC's, TABLE2 the whole table (RVP.EXE's: the engine and the play tests), TABLE
SRC's alone (the other EXEs'): an engine module's tokens are the same in both.

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


# a play test's message argument: (the call, the argument's index)
MESSAGE_ARGS = {'REQUIRE': 1, 'EXPECT': 1, 'pt_fail': 0, 'ln_f': 0}


def message_args(code):
    """the places of the bare names given as a play test's message argument"""
    at = set()
    for m in re.finditer(r'\b(%s)\s*\(' % '|'.join(MESSAGE_ARGS), code):
        want, i, depth, k, start = MESSAGE_ARGS[m.group(1)], m.end(), 0, 0, m.end()
        while i < len(code):
            c = code[i]
            if c == '"':
                i = literal_end(code, i)
                continue
            if c == "'":
                i += len(re.match(r"'(?:[^'\\\n]|\\.)*'", code[i:]).group(0))
                continue
            if c in '([{':
                depth += 1
            elif c in ')]}' and depth:
                depth -= 1
            elif (c == ',' or c == ')') and not depth:
                if k == want:
                    a = re.fullmatch(r'\s*(\w+)\s*', code[start:i])
                    if a:
                        at.add(start + a.start(1))
                    break
                if c == ')':
                    break
                k, start = k + 1, i + 1
            i += 1
    return at


def transform(src, table):
    code = strip_comments(src)
    messages = message_args(code)
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
            # (another `NAME[] = ...` is a declaration of the same name in another function's scope, not a use)
            uses = [u.start() for u in re.finditer(r'\b%s\b(?!\s*\[\s*\]\s*=)' % re.escape(name), code)]
            if uses and all(u in messages or re.search(r'\b(?:rv_logf|rv_notef)\s*\(\s*$', code[max(0, u - 40):u])
                            for u in uses):
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


def write(table, table_path):
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
    print('logfmt: %s: %d formats, %d bytes of text out of memory (%d bytes of tokens in its place), table %d bytes'
          % (os.path.basename(table_path), len(table), texts, tokens, len(blob)))


def main():
    if sys.argv[1] == '--append':
        append(sys.argv[2], sys.argv[3])
        return
    outdir, table_path, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    srcs, then = (rest[:rest.index('--then')], rest[rest.index('--then') + 1:]) if '--then' in rest else (rest, [])
    os.makedirs(outdir, exist_ok=True)
    table = []
    for group, path_out in ((srcs, table_path), (then[1:], then[0] if then else None)):
        for path in group:
            out, n = transform(open(path, encoding='latin-1').read(), table)
            open(os.path.join(outdir, os.path.basename(path)), 'w', encoding='latin-1').write(out)
        if path_out:
            write(table, path_out)


main()
