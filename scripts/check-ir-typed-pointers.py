#!/usr/bin/env python3
"""Static guard against typed-pointer mismatches in text-backend .ll files.

The text backend (HAVE_LLVM=0) emits LLVM IR with *typed* pointers (i8*, i64*,
[3 x i8*]*, ...). The opaque-pointer clang on the local dev machine and on the
ubuntu/macos CI lanes folds every pointer to `ptr` and accepts any spelling, but
bookworm ships LLVM 14, which still enforces typed pointers -- a `store i8* %v,
i8* %p` where `%p` is `alloca i8**` is rejected there. Those mismatches are the
exact family that broke the bookworm lane (see CHANGELOG "Text-IR backend
declaration / call-site type family").

This script emulates the LLVM 14 type check for the operations the text backend
actually emits, so the class of bug is caught *locally* (and before any push),
not only via a 5-commit CI round-trip.

Scope is deliberately narrow and conservative: it tracks pointer-pointee types
for allocas, globals, GEPs and bitcasts, and verifies store/load pointer
operands. It does NOT attempt a full LLVM type system -- anything it cannot prove
is simply skipped, never flagged. That keeps false positives at zero on the real
corpus.

Usage: scripts/check-ir-typed-pointers.py FILE.ll [FILE.ll ...]
       0 = clean, 1 = at least one type clash found.
"""

import re
import sys

# Parse "<mode> <type> <sha>\t<path>" style ls-tree entry, or "<ty> <reg> = ..."
PAT_TYPE_TOKEN = re.compile(r"^[a-zA-Z_][\w.$]*\*?$")


def split_fields(s):
    """Split a `{ a, b, c }` field list on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Checker:
    def __init__(self):
        # reg -> pointee type, e.g. "%p" -> "i8*"  (the type stored THROUGH it)
        self.pointee = {}
        # reg -> full type for bitcast results, e.g. "%r" -> "i8**"
        self.full = {}
        # name -> [field types] for struct defs
        self.structs = {}
        self.errors = []

    def note_pointee(self, reg, ty):
        if reg.startswith("%") or reg.startswith("@"):
            self.pointee[reg] = ty

    def note_full(self, reg, ty):
        if reg.startswith("%") or reg.startswith("@"):
            self.full[reg] = ty

    def check(self, path):
        text = open(path, encoding="utf-8", errors="replace").read()

        for m in re.finditer(r"^%[\w.$]+\s*=\s*type\s*\{([^}]*)\}", text, re.M | re.S):
            self.structs[m.group(0).split("=", 1)[0].strip()] = split_fields(m.group(1))

        for m in re.finditer(r"^\s*([%@][\w.$]+)\s*=\s*alloca\s+([^\s,]+)\s*$", text, re.M):
            self.note_pointee(m.group(1), m.group(2) + "*")
        for m in re.finditer(r"^\s*(@[\w.$]+)\s*=\s*(?:global|constant)\s+([^\s,]+)", text, re.M):
            self.note_pointee(m.group(1), m.group(2) + "*")

        for m in re.finditer(
            r"^([%@][\w.$]+)\s*=\s*getelementptr\s+(?:inbounds\s+)?"
            r"([^\s,]+)\s*,\s*([^\s,]+)\s+([%@][\w.$]+)\s*(?:,\s*(.*))?$",
            text, re.M,
        ):
            reg, elem, pty, src, rest = m.groups()
            want = self.pointee.get(src)
            if want and pty != want:
                self.errors.append(
                    f"{path}: gep src {src} typed {pty}, expected {want} :: "
                    f"{m.group(0).strip()[:110]}"
                )
            cur = elem
            if rest:
                idxs = re.findall(r"(?:i32|i64)\s+([-\d]+|%[\w.$]+)", rest)
                for i, ix in enumerate(idxs):
                    if i == 0:
                        continue  # pointer index keeps the element type
                    if ix.startswith("%"):
                        cur = None
                        break
                    if cur and cur.startswith("["):
                        mm = re.match(r"\[[^\]]*\]\s*(.*)$", cur)
                        cur = mm.group(1) if mm else None
                    elif cur and cur.startswith("%") and cur in self.structs:
                        f = self.structs[cur]
                        k = int(ix)
                        cur = f[k] if k < len(f) else None
                    else:
                        cur = None
                        break
            if cur:
                self.note_pointee(reg, cur + "*")

        for m in re.finditer(
            r"^([%@][\w.$]+)\s*=\s*bitcast\s+([^\s]+)\s+([%@][\w.$]+)\s+to\s+([^\s]+)\s*$",
            text, re.M,
        ):
            reg, srcty, src, dstty = m.groups()
            want = self.pointee.get(src)
            if want and srcty != want:
                self.errors.append(
                    f"{path}: bitcast src {src} typed {srcty}, expected {want} :: "
                    f"{m.group(0).strip()[:110]}"
                )
            self.note_full(reg, dstty)

        for m in re.finditer(r"^\s*(?:[%@][\w.$]+\s*=\s*)?(store|load)\s+(.+)$", text, re.M):
            op, body = m.group(1), m.group(2)
            if op == "store":
                sm = re.match(
                    r"([^\s,]+)\s+(?:bitcast\s*\(.*?\)\s*to\s+[^\s,]+|[%@][\w.$]+|"
                    r"true|false|null|[^\s,]+?),\s*([^\s,]+)\s+([%@][\w.$]+)\s*$",
                    body,
                )
                if not sm:
                    continue
                ptrty, ptr = sm.group(2), sm.group(3)
            else:
                lm = re.match(r"([^\s,]+)\s*,\s*([^\s,]+)\s+([%@][\w.$]+)\s*$", body)
                if not lm:
                    continue
                ptrty, ptr = lm.group(2), lm.group(3)
            # bitcast results carry a full type; resolve through it
            want = self.pointee.get(ptr)
            if not want and ptr in self.full:
                want = self.full[ptr]
            if want and ptrty != want:
                self.errors.append(
                    f"{path}: {op} via {ptr} typed {ptrty}, expected {want} :: "
                    f"{body[:110]}"
                )
        return self.errors


def main():
    if len(sys.argv) < 2:
        print("usage: check-ir-typed-pointers.py FILE.ll [FILE.ll ...]", file=sys.stderr)
        return 2
    all_errs = []
    for p in sys.argv[1:]:
        all_errs += Checker().check(p)
    for e in all_errs:
        print(e)
    print(f"=== checked {len(sys.argv) - 1} file(s), {len(all_errs)} error(s) ===")
    return 1 if all_errs else 0


if __name__ == "__main__":
    sys.exit(main())
