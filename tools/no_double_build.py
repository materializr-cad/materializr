#!/usr/bin/env python3
"""Fail if an OCCT boolean is performed twice for one result.

The two-shape constructor of BRepAlgoAPI_Cut/Fuse/Common is marked "Obsolete"
in OCCT's own header and it PERFORMS the operation. So this shape

    BRepAlgoAPI_Cut cut(body, tool);
    cut.SetFuzzyValue(1.0e-4);
    cut.Build();

runs the whole boolean twice for a byte-identical result, and every setter
written between the two lines reaches only the second run. Both halves are
invisible at review time: the code reads exactly like the correct thing.

Thirty-one sites had drifted into it before this gate existed, costing a
second full boolean each (927 ms per cut on the 300-hole plate). The results
were correct: Shape() returns the SECOND build, which did have the setters
applied. What was wasted is the first build, which ran at the default
tolerance and was then thrown away. Use materializr::setBooleanShapes from
modeling/BoolArgs.h, which constructs empty and declares the operands, so one
Build() runs one boolean with the setters in effect.

A two-shape constructor with NO trailing Build() is fine and not reported:
that is a single build, just written through the obsolete spelling. Nor is
BRepAlgoAPI_Section with PerformNow=false, whose constructor is explicitly
told not to build, so its Build() is the only one.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CTOR = re.compile(r'BRepAlgoAPI_(Cut|Fuse|Common|Section)\s+([A-Za-z_]\w*)\s*\(', re.S)


def statement_end(text, open_paren):
    """Index of the ')' closing the argument list that starts at open_paren."""
    depth = 0
    for i in range(open_paren, len(text)):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return i
    return None


def top_level_arg_count(args):
    depth = 0
    n = 1
    for ch in args:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ',' and depth == 0:
            n += 1
    return n


def strip_comments(lines):
    """Blank out // comments and /* */ blocks, keeping line numbering."""
    out = []
    in_block = False
    for line in lines:
        res = ''
        i = 0
        while i < len(line):
            two = line[i:i + 2]
            if in_block:
                if two == '*/':
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if two == '/*':
                in_block = True
                i += 2
                continue
            if two == '//':
                break
            res += line[i]
            i += 1
        out.append(res)
    return out


def scope_tail(text, pos):
    """text from pos to the closing brace of the block enclosing pos."""
    depth = 0
    for i in range(pos, len(text)):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            if depth == 0:
                return text[pos:i]
            depth -= 1
    return text[pos:]


def scan(path):
    # Matched over the whole comment-stripped text, not line by line: a
    # declaration wrapped across lines is still one statement, and a
    # commented-out Build must not make a live constructor look doubled.
    raw = path.read_text(encoding='utf-8', errors='replace').split('\n')
    text = '\n'.join(strip_comments(raw))
    out = []
    for m in CTOR.finditer(text):
        var = m.group(2)
        start = m.end() - 1          # the '(' the regex just matched
        end = statement_end(text, start)
        if end is None:
            continue
        args = text[start + 1:end]
        if top_level_arg_count(args) < 2:
            continue          # already the empty-construct form
        if m.group(1) == 'Section' and 'Standard_False' in args:
            continue          # PerformNow=false: the constructor does not build
        # Search only to the end of the block that DECLARES the variable. A
        # fixed line window runs past the closing brace and finds a same-named
        # variable's Build in a later scope, which flags a constructor that
        # never had one.
        after = scope_tail(text, end + 1)
        if re.search(re.escape(var) + r'\s*\.\s*Build\s*\(', after):
            out.append((text[:m.start()].count('\n') + 1, var))
    return out


# The detector's own cases. It grew real logic - comment stripping, scope
# bounding, wrapped declarations, Section's PerformNow - and every rule here
# is one a reviewer found the hard way, so they are pinned.
SELFTEST = """
void a() { BRepAlgoAPI_Cut cut(body, tool); cut.Build(); }

void b() { BRepAlgoAPI_Cut cut(body, tool); cut.Build(range); }

void c() { BRepAlgoAPI_Cut
               cut(body, tool);
           cut.Build(); }

void d() { BRepAlgoAPI_Cut cut(body, tool); /* cut.Build(); */ }

void e() { BRepAlgoAPI_Cut cut(body, tool);
           // cut.Build();
         }

void f() { BRepAlgoAPI_Section sec(x, y, Standard_False); sec.Build(); }

void g() { BRepAlgoAPI_Cut cut; setBooleanShapes(cut, body, tool); cut.Build(); }

void h() { BRepAlgoAPI_Cut cut(body, tool); }

void i() { try {} catch (...) { BRepAlgoAPI_Cut cut(body, tool); cut.Build(); } }
"""

SELFTEST_EXPECT = [
    ('a', 'plain double build', True),
    ('b', 'Build() carrying a progress range', True),
    ('c', 'declaration wrapped across lines', True),
    ('d', 'the Build sits in a /* */ comment', False),
    ('e', 'the Build sits in a // comment', False),
    ('f', 'Section with PerformNow=false', False),
    ('g', 'the correct empty-construct form', False),
    ('h', 'two-shape constructor with no Build at all', False),
    ('i', 'constructor is not first on its line', True),
]


def selftest():
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / 'cases.cpp'
        f.write_text(SELFTEST)
        lines = SELFTEST.split('\n')
        flagged = {ln for ln, _ in scan(f)}
        for name, what, want in SELFTEST_EXPECT:
            start = next(i for i, l in enumerate(lines, 1)
                         if l.startswith(f'void {name}('))
            end = next((i for i, l in enumerate(lines, 1)
                        if i > start and l.strip() == ''), len(lines))
            got = any(start <= h < end for h in flagged)
            if got != want:
                ok = False
            print(f"  {'ok  ' if got == want else 'FAIL'}  {name}: {what} "
                  f"-> {'flagged' if got else 'clean'}"
                  f"{'' if got == want else ' (WRONG)'}")
    print('detector self-test passed' if ok else 'DETECTOR SELF-TEST FAILED')
    return 0 if ok else 1


def main():
    if '--selftest' in sys.argv:
        return selftest()

    hits = []
    for path in sorted((ROOT / 'src').rglob('*')):
        if path.suffix in ('.cpp', '.h', '.hxx'):
            for ln, var in scan(path):
                hits.append((path.relative_to(ROOT), ln, var))
    if not hits:
        print('no double-built booleans')
        return 0
    print('Boolean performed twice for one result '
          '(two-shape constructor plus an explicit Build):\n')
    for rel, ln, var in hits:
        print(f'  {rel}:{ln}  {var}')
    print('\nConstruct empty and use materializr::setBooleanShapes '
          '(modeling/BoolArgs.h),\nso the single Build() runs one boolean '
          'with the setters in effect.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
