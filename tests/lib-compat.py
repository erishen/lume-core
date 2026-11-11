#!/usr/bin/env python3
"""P16.1 — keep the shipped `lib/*.lume` loadable by a legacy lume 0.12.2.

lume 0.12.2 (the released binary, before the for-init scoping fix) registers
a `let` inside the block that *contains* the declaration, including a
`for (let i ...)` header. Two such declarations in the same block abort the
whole import with "duplicate declaration of 'i' in the same scope". lume 0.13
has proper for-init block scoping, and it also rejects an inner loop that
shadows an outer local, so the only portable fix is a unique name per loop.

Two independent legs:

  1. STATIC (always runs, including CI): a brace-aware, string/comment-masked
     scan of every `lib/*.lume` for duplicate `let` names in the same block.
     A self-check feeds a known-bad sample through the same matcher so a blind
     pattern cannot turn the gate green. This is the authoritative leg when no
     0.12 binary is installed.
  2. DYNAMIC (when a *legacy* 0.12.2 binary is available): actually import
     `sci.lume`, `optim.lume`, `linalg.lume` and evaluate four known values.
     A candidate binary must first FAIL a probe containing two `for (let i)`
     loops in one block — a 0.12 build that already has the for-init scoping
     fix cannot reproduce the bug and would make this leg falsely green.

The dynamic leg deliberately does not cover `complex`, `dist`, `sig`, `pde`
or `ml`: those hit unrelated 0.12 gaps (missing `atan2` / `norminv` / `sin`
builtins, or a parser difference), not duplicate declarations. See TODO.md
P16.1.
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIB = ROOT / "lib"
SHIPPED = ("sci.lume", "optim.lume", "linalg.lume")
# 注：blas.lume 不在 SHIPPED——它依赖新内建 matmul()，legacy 0.12.2 宿主
# 没有该内建（墙 3 的 C kernel 快路径·见 lib/blas.lume 头注）。静态 P16.1
# 重复 let 扫描仍覆盖全部 lib/*.lume（含 blas.lume）。

# Two `for (let i)` in the same block: the exact shape 0.12.2 rejects.
BAD_SAMPLE = """func f() {
  for (let i = 0; i < 2; i = i + 1) { print(i); }
  for (let i = 0; i < 2; i = i + 1) { print(i); }
}
f();
"""

VALUES = """import "{sci}" as sci;
import "{optim}" as opt;
import "{linalg}" as la;
print("entropy", sci.entropy([0.5, 0.5]));
print("poly_fit", opt.poly_fit([0.0, 1.0, 2.0], [1.0, 3.0, 5.0], 1));
print("lp", opt.linprog_simplex([3.0, 2.0], [[1.0, 1.0], [2.0, 1.0]], [4.0, 5.0]));
print("qr", la.mat_qr_q([[1.0, 2.0], [3.0, 4.0]]));
"""

EXPECTED = (
    "entropy 1",
    "poly_fit [1,2]",
    "lp [1,3]",
    "qr [[0.316228,0.948683],[0.948683,-0.316228]]",
)


def fail(msg):
    print("FAIL lib-compat: " + msg)
    sys.exit(1)


def mask(src):
    """Blank out comments and string literals, keeping offsets and newlines."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if src[k] != "\n":
                    out[k] = " "
            i = j
        elif c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if src[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def block_duplicates(src):
    """Return [(name, [line, ...])] for names declared twice in one block.

    A `for (let i ...)` header sits BEFORE the loop body's `{`, so it belongs
    to the enclosing block — which is exactly 0.12.2's rule. Map/struct braces
    also push/pop a block, but never contain a `let`, so they are harmless.
    """
    m = mask(src)
    stack = []
    decls = []
    for mm in re.finditer(r"[{}]|\blet\s+([A-Za-z_]\w*)", m):
        tok, name = mm.group(0), mm.group(1)
        if tok == "{":
            stack.append(mm.start())
        elif tok == "}":
            if stack:
                stack.pop()
        else:
            decls.append((tuple(stack), name, src.count("\n", 0, mm.start()) + 1))
    by_block = {}
    for block, name, line in decls:
        by_block.setdefault(block, {}).setdefault(name, []).append(line)
    out = []
    for names in by_block.values():
        for name, lines in names.items():
            if len(lines) > 1:
                out.append((name, lines))
    return out


def static_leg():
    files = sorted(LIB.glob("*.lume"))
    if len(files) < len(SHIPPED):
        fail("lib/ is missing shipped libraries")
    if not block_duplicates(BAD_SAMPLE):
        fail("static matcher is blind (self-check)")
    bad = []
    for p in files:
        for name, lines in block_duplicates(p.read_text()):
            bad.append("    %s: '%s' at lines %s" % (p.name, name, lines))
    if bad:
        fail("duplicate let in the same block (0.12.2 would refuse the import):\n"
             + "\n".join(bad)
             + "\n    Rename each repeated loop variable; do not drop the `let`"
               " (0.13 then reports an undefined variable).")
    print("ok   lib-compat: %d lib files, 0 duplicate let per block"
          " (matcher self-check ok)" % len(files))


def run(argv):
    try:
        r = subprocess.run([str(a) for a in argv], capture_output=True,
                           text=True, timeout=30)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except (OSError, subprocess.TimeoutExpired):
        return None, ""


def candidates():
    out = []
    for key in ("LUME_012", "LUME"):
        v = os.environ.get(key)
        if v:
            out.append(pathlib.Path(v))
    out.append(pathlib.Path.home() / ".local/bin/lume")
    out.append(ROOT.parent / "lume" / "bin" / "lume")  # sibling host source tree
    found = shutil.which("lume")
    if found:
        out.append(pathlib.Path(found))
    return out


def legacy_baseline(td):
    """A 0.12 binary that still reproduces the bug, or None."""
    probe = pathlib.Path(td) / "baseline-probe.lume"
    probe.write_text(BAD_SAMPLE)
    for b in candidates():
        if not b.is_file() or not os.access(b, os.X_OK):
            continue
        rc, out = run([b, "--version"])
        if rc != 0 or "0.12" not in out:
            continue
        rc, out = run([b, probe])
        if rc and "duplicate declaration" in out:
            return b
        print("note lib-compat: %s is 0.12 but already has for-init scoping;"
              " not a legacy baseline" % b)
    return None


def dynamic_leg():
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        b = legacy_baseline(td)
        if b is None:
            print("skip lib-compat: no legacy 0.12.2 binary (CI has none);"
                  " static leg is authoritative")
            return
        for name in SHIPPED:
            f = d / ("probe-" + name)
            f.write_text('import "%s" as m;\nprint("import-ok");\n' % (LIB / name))
            rc, out = run([b, f])   # --check does NOT load imports; run it.
            if rc != 0 or "import-ok" not in out:
                fail("legacy 0.12.2 could not import %s:\n%s" % (name, out.strip()))
        vals = d / "values.lume"
        vals.write_text(VALUES.format(sci=LIB / "sci.lume",
                                      optim=LIB / "optim.lume",
                                      linalg=LIB / "linalg.lume"))
        rc, out = run([b, vals])
        if rc != 0:
            fail("legacy 0.12.2 known-value script failed:\n%s" % out.strip())
        for want in EXPECTED:
            if want not in out:
                fail("legacy 0.12.2 known value missing: %r\n%s" % (want, out.strip()))
        print("ok   lib-compat: legacy 0.12.2 baseline loaded 3 shipped libs"
              " + 4 known values (%s)" % b)


def main():
    static_leg()
    dynamic_leg()
    print("==> lib-compat: P16.1 gate holds")


if __name__ == "__main__":
    main()
