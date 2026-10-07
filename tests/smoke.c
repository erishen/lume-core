#include "lume.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Headless interpreter/runtime unit tests (no HTTP needed). Each snippet is
 * parsed, type checked, executed against a fresh VM, and its stdout (from
 * print()) is compared byte-for-byte against the expected output. */

static int tests_run = 0;
static int tests_failed = 0;
static int saved_stdout = -1;

static int capture_begin(char *path) {
    snprintf(path, 256, "/tmp/lume-smoke-%d.out", (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    return fd;
}

static void capture_end(int fd, char *path, char *out, size_t cap) {
    fflush(stdout);
    if (saved_stdout >= 0) {  /* restore the original stdout */
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
        saved_stdout = -1;
    }
    close(fd);
    int rfd = open(path, O_RDONLY);
    ssize_t n = rfd >= 0 ? read(rfd, out, cap - 1) : -1;
    if (rfd >= 0) close(rfd);
    out[n < 0 ? 0 : n] = '\0';
}

/* Hand one test's object graph back: the AST (node_free recurses through
 * everything the parser built), the GC heap (vm_free), and the Type graph the
 * parser handed to the checker. Every test releases before returning, so the
 * leak report names the process, not one snippet. */
static void smoke_release(Node *prog, VM *vm) {
    vm_free(vm);
    node_free(prog);
    type_release_all();
}

static void check(const char *name, const char *src, const char *expect) {
    tests_run++;
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    VM vm;
    if (!prog) {
        fprintf(stderr, "FAIL %-32s parse: %s\n", name, err);
        tests_failed++;
        return;
    }
    if (!type_check_program(prog, err, sizeof(err))) {
        fprintf(stderr, "FAIL %-32s typecheck: %s\n", name, err);
        tests_failed++;
        smoke_release(prog, NULL);
        return;
    }
    vm_init(&vm);
    char path[256];
    int fd = capture_begin(path);
    exec_program(&vm, prog);
    char out[8192];
    capture_end(fd, path, out, sizeof(out));
    unlink(path);

    if (vm.error) {
        fprintf(stderr, "FAIL %-32s runtime: %s\n", name, vm.error_msg);
        tests_failed++;
    } else if (strcmp(out, expect) != 0) {
        fprintf(stderr, "FAIL %-32s output\n  got: %s\n want: %s\n", name, out,
                expect);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }
    smoke_release(prog, &vm);
}

/* The snippet must be REJECTED by the type checker with `want_sub` in the
 * message (proves the strong-typing pass actually catches things). */
static void reject(const char *name, const char *src, const char *want_sub) {
    tests_run++;
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    bool accepted = prog && type_check_program(prog, err, sizeof(err));
    if (accepted || !strstr(err, want_sub)) {
        fprintf(stderr,
                "FAIL %-32s expected type error containing '%s'; got: %s\n",
                name, want_sub, accepted ? "accepted (no error)" : err);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }
    smoke_release(prog, NULL);
}

/* The --no-fs lock (VM.no_fs) that check_err_nofs() turns on for one run.
 * Declared here, above check_err, because check_err applies it to the VM it
 * sets up; the driving helper sits below check_err's definition. */
static bool g_no_fs = false;
static bool g_no_net = false;
static bool g_http_lock_net = false;

/* The snippet must fail AT RUNTIME with `want_sub` in the VM error. This is
 * the other half of `reject`: type errors are caught by the checker, but the
 * math/domain guards run inside the native functions (e.g. sqrt of a
 * negative), so those paths would otherwise be untested. */
static void check_err(const char *name, const char *src, const char *want_sub) {
    tests_run++;
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    VM vm;
    if (!prog) {
        fprintf(stderr, "FAIL %-32s parse: %s\n", name, err);
        tests_failed++;
        return;
    }
    if (!type_check_program(prog, err, sizeof(err))) {
        fprintf(stderr, "FAIL %-32s typecheck: %s\n", name, err);
        tests_failed++;
        smoke_release(prog, NULL);
        return;
    }
    vm_init(&vm);
    vm.no_fs = g_no_fs;
    vm.no_net = g_no_net;
    char path[256];
    int fd = capture_begin(path);
    exec_program(&vm, prog);
    char out[8192];
    capture_end(fd, path, out, sizeof(out));
    unlink(path);

    const char *msg = vm.error_msg; /* a plain char[], never NULL */
    if (!vm.error || !strstr(msg, want_sub)) {
        fprintf(stderr,
                "FAIL %-32s expected runtime error containing '%s'; got: %s\n",
                name, want_sub, msg);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }
    smoke_release(prog, &vm);
}

/* The --no-fs lock (VM.no_fs) applies for the duration of one check_err run.
 * A file is set rather than a parameter because check_err's callers are
 * one-liners and adding a flag argument would touch all of them; check_err
 *_nofs() sets and always clears it, so a partial run cannot leak the lock
 * into the next assertion. */
static void check_err_nofs(const char *name, const char *src, const char *want_sub) {
    g_no_fs = true;
    check_err(name, src, want_sub);
    g_no_fs = false;
}

/* The --no-net lock (VM.no_net) for one check_err run, same discipline as the
 * --no-fs one. */
static void check_err_nonet(const char *name, const char *src, const char *want_sub) {
    g_no_fs = false;
    g_no_net = true;
    check_err(name, src, want_sub);
    g_no_net = false;
}

/* http_get 的离线可判定路径:完整跑一段脚本,拿到 print() 的stdout 逐字节比
 * 对。两条用例都不需要出网:
 *
 *  - `--no-net` 总闸:http_get 第一件事就是查 no_net,直接给运行时错误。
 *  - SSRF 闸门:私有地址在 open_conn 之前就被拒,socket 根本不开,
 *    所以这条断言在无网/CI 里同样确定(不依赖 DNS 与出网)。
 *
 * 期望输出里只断言 err 的要点,别把整句贴进去(措辞会跟着改)。 */
static void check_http_offline(const char *name, const char *src,
                               const char *expect) {
    tests_run++;
    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    if (!prog) {
        fprintf(stderr, "FAIL %-32s parse: %s\n", name, err);
        tests_failed++;
        return;
    }
    if (!type_check_program(prog, err, sizeof(err))) {
        fprintf(stderr, "FAIL %-32s typecheck: %s\n", name, err);
        tests_failed++;
        smoke_release(prog, NULL);
        return;
    }
    VM vm;
    vm_init(&vm);
    vm.no_net = g_http_lock_net;  /* 由调用方决定这次是不是走总闸 */
    char path[256];
    int fd = capture_begin(path);
    exec_program(&vm, prog);
    char out[8192];
    capture_end(fd, path, out, sizeof(out));
    unlink(path);
    if (vm.error || strcmp(out, expect) != 0) {
        fprintf(stderr, "FAIL %-32s output%s\n  got: %s\n want: %s\n", name,
                vm.error ? " (vm error)" : "", out, expect);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }
    smoke_release(prog, &vm);
}


/* Multi-file module tests (import/export): write `files` (name -> content)
 * under /tmp/lume-smoke-modules/<name>/, then run `entry` through the loader
 * (parse + typecheck + execute, dependencies first, each module once). When
 * `want_fail` is non-NULL the run must fail with that substring in the error;
 * otherwise stdout (from print()) is compared byte-for-byte. */
static void check_modules(const char *name, const char *const files[][2],
                          int nfiles, const char *entry, const char *expect,
                          const char *want_fail) {
    tests_run++;
    /* The VM is set up before the scratch files exist, not after: every exit
     * below (dir creation, fopen, and above all the `want_fail` verdicts) has
     * to reach the teardown at `done:`, and a goto cannot land past an
     * initialiser it skipped. */
    VM vm;
    vm_init(&vm);
    char err[512] = {0};
    char path[256];
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/lume-smoke-modules/%s", name);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "FAIL %-32s cannot create scratch dir\n", name);
        tests_failed++;
        goto done;
    }
    for (int i = 0; i < nfiles; i++) {
        char p[320];
        snprintf(p, sizeof(p), "%s/%s", dir, files[i][0]);
        FILE *f = fopen(p, "wb");
        if (!f) {
            fprintf(stderr, "FAIL %-32s cannot write %s\n", name, files[i][0]);
            tests_failed++;
            goto done;
        }
        fputs(files[i][1], f);
        fclose(f);
    }

    char entrypath[320];
    snprintf(entrypath, sizeof(entrypath), "%s/%s", dir, entry);
    int fd = capture_begin(path);
    int rc = loader_run(&vm, entrypath, false, err, sizeof(err));
    char out[8192];
    capture_end(fd, path, out, sizeof(out));
    unlink(path);

    if (want_fail) {
        if (rc == 0 || !strstr(err, want_fail)) {
            fprintf(stderr, "FAIL %-32s expected loader error containing '%s'; "
                            "rc=%d got: %s\n", name, want_fail, rc, err);
            tests_failed++;
        } else {
            printf("ok   %s\n", name);
        }
        goto done;
    }
    if (rc != 0) {
        fprintf(stderr, "FAIL %-32s loader: %s\n", name, err);
        tests_failed++;
    } else if (vm.error) {
        fprintf(stderr, "FAIL %-32s runtime: %s\n", name, vm.error_msg);
        tests_failed++;
    } else if (strcmp(out, expect) != 0) {
        fprintf(stderr, "FAIL %-32s output\n  got: %s\n want: %s\n", name, out,
                expect);
        tests_failed++;
    } else {
        printf("ok   %s\n", name);
    }

done:
    /* The loader owns the modules, so it goes first: it drops the AST, then
     * vm_free empties the heap the executed module top levels filled, then
     * type_release_all sweeps the Type graph. Every path above lands here. */
    loader_free(&vm);
    vm_free(&vm);
    type_release_all();
}

/* Recreate the files()/read_file() fixture under /tmp and return 0, or -1
 * if the directory or the file could not be made.
 *
 * This used to probe the repo's own `docs/` and `docs/LUME.md`, which
 * pinned the whole check to the repository root: started from anywhere else,
 * `files("docs")` is an empty list and the case reported
 * "128 tests, 1 failed" even though the builtins were fine — CI stayed green
 * only because it runs from the checkout root. Every other filesystem case
 * here already builds its fixture under /tmp, so this one does too and the
 * verdict stops depending on the cwd.
 *
 * The payload is deliberately longer than the 1000 bytes the case asserts on,
 * so the length edge is preserved rather than lowered. */
static int smk_fs_fixture(void) {
    const char *dir = "/tmp/lume-smoke-files";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "FAIL %-32s cannot create fixture dir\n",
                "files: sorted listing");
        tests_failed++;
        return -1;
    }
    char path[320];
    snprintf(path, sizeof(path), "%s/LUME.md", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "FAIL %-32s cannot create fixture file\n",
                "files: sorted listing");
        tests_failed++;
        return -1;
    }
    for (int i = 0; i < 2048; i++) {
        if (fputc('x', f) == EOF) {
            fclose(f);
            fprintf(stderr, "FAIL %-32s cannot fill fixture file\n",
                    "files: sorted listing");
            tests_failed++;
            return -1;
        }
    }
    fclose(f);
    return 0;
}

int main(void) {
    /* The fork has no agent-httpd tool/skill tables, so tools() and skills()
     * were never seeded here. mcps() below resets its own catalog instead. */

    /* mcps() reads <cwd>/.data/mcp-servers-router.json — the artifact a
     * router sync writes. Resetting it here keeps the check deterministic
     * even after manual runs have dropped a real catalog. */
    mkdir(".data", 0755);
    unlink(".data/mcp-servers-router.json");

    check("arithmetic + casts",
          "print(str(1 + 2 * 3)); print(str(int(\"42\") - 2.5));",
          "7\n39.5\n");

    /* int is i64 (docs/SPEC.md#integers). It used to be carried in a C double,
     * so anything past 2^53 was rounded away and overflow saturated instead of
     * wrapping — and the native backends, which work in i64, disagreed. The
     * interpreter half of that is pinned here; the three-way version lives in
     * tests/native-consistency.lume. */

    /* 2^53+1 is the smallest integer a double cannot represent, so it is the
     * smallest witness for "the lexer must not route ints through strtod". */
    check("int literal past 2^53 is exact",
          "print(str(9007199254740993)); print(str(123456789012345678));",
          "9007199254740993\n123456789012345678\n");

    check("int survives print() and str()",
          "print(9223372036854775807); print(-9223372036854775808);",
          "9223372036854775807\n-9223372036854775808\n");

    /* Wrapping, matching the emitters' unadorned `add i64` / `mul i64`. */
    check("int overflow wraps as i64",
          "let m = 9223372036854775807; print(str(m + 1)); print(str(m * 2));"
          " print(str(0 - m - 1));",
          "-9223372036854775808\n-2\n-9223372036854775808\n");

    /* int %-stays integer (never fmod's double) and keeps C's sign; `/` is
     * documented as floating point even for two ints. */
    check("int modulo keeps C signs, / is float",
          "print(str(-7 / 2)); print(str(-7 % 3)); print(str(7 % -3));",
          "-3.5\n-1\n1\n");

    /* Unary minus must keep int an int: it used to widen through double, so
     * `-7 % 3` was answered with "% does not apply to floats" even though the
     * type checker types `-7` as int. */
    check("unary minus keeps int an int",
          "let a = -7; print(str(a)); print(str(a % 3)); print(str(-7 + 0));",
          "-7\n-1\n-7\n");

    /* Negating the i64 minimum wraps to itself, consistent with 1.2. */
    check("negating the i64 minimum wraps to itself",
          "let x = -9223372036854775808; print(str(0 - x));",
          "-9223372036854775808\n");

    /* `%` on a float is refused by all three backends, never silently fmod'd. */
    check_err("modulo on a float is a runtime error",
              "print(str(5.5 % 2));",
              "does not apply to floats");

    check("int/float compare across kinds",
          "print(str(1 == 1.0)); print(str(2.5 + 1)); print(str(1 < 1.5));",
          "true\n3.5\ntrue\n");

    check("large int as a map key round-trips",
          "let m = { \"id\": 9007199254740993 }; print(str(get(m, \"id\", 0)));",
          "9007199254740993\n");

    /* The negative minimum is the one literal whose magnitude alone overflows
     * i64, so it needs the unary minus to become representable. */
    check("negative i64 minimum is the minimum, not a wrap",
          "let x = -9223372036854775808; print(str(x)); print(str(x - 1));",
          "-9223372036854775808\n9223372036854775807\n");

    /* Out of range in either direction, and 2^63 with nothing to negate it:
     * each must be refused rather than silently rounded. */
    reject("int literal beyond i64 is refused",
           "let x = 99999999999999999999;",
           "does not fit in int");

    reject("bare 2^63 is refused",
           "let x = 9223372036854775808;",
            "does not fit in int");

    /* ---- builtins: the semantics docs/SPEC.md §5 pins down -------------
     * These are the ones that surprise people, so they get an executable
     * claim rather than only a doc line. A silent 0 or a flipped argument
     * order is exactly what a reader cannot check by skimming. */

    check("get: missing key is null, with default is the default",
          "print(str(get({a:1}, \"b\"))); print(str(get({a:1}, \"b\", 99)));",
          "null\n99\n");

    /* range's upper bound is exclusive. */
    check("range excludes its upper bound",
          "print(str(range(1, 5)));",
          "[1,2,3,4]\n");

    /* put/push mutate in place AND return the collection (not null), so they
     * chain. Documented in SPEC 5.2 after this was first written as null. */
    check("put/push return the collection, not null",
          "print(str(put({a:1}, \"b\", 2))); print(str(push([1], 2)));",
          "{\"a\":1,\"b\":2}\n[1,2]\n");

    /* len on a non-collection answers 0 rather than failing. */
    check("len of a non-collection is silently 0",
          "print(str(len(5)));",
          "0\n");

    check_err("keys() rejects a list",
              "print(str(keys([1])));",
              "keys() expects a map");

    /* The higher-order builtins take (fn, list) — the reverse of most
     * languages, and getting it wrong is a type error, not a silent swap. */
    check("map/filter/reduce take (fn, list)",
          "print(str(map((x) => x * x, [1,2,3])));"
          " print(str(filter((x) => x > 1, [1,2,3])));"
          " print(str(reduce((a, b) => a + b, [1,2,3], 0)));",
          "[1,4,9]\n[2,3]\n6\n");

    check_err("map() rejects the (list, fn) order",
              "print(str(map([1,2,3], (x) => x)));",
              "map() expects (fn, list)");

    check("try(fn) reports ok/err instead of propagating",
          "print(str(try(() => 42))); print(str(try(() => 1 / 0)));",
          "{\"ok\":42,\"err\":null}\n{\"ok\":null,\"err\":\"division by zero\"}\n");

    /* Conversions fail silently: a mistyped field name becomes 0. */
    check("int()/float() of a non-numeric string is silently 0",
          "print(str(int(\"abc\"))); print(str(float(\"x\"))); print(str(int(\"3.9\")));",
          "0\n0\n3\n");

    check("read_file of a missing path is null, env of an unset var is null",
          "print(str(read_file(\"nope-does-not-exist.txt\")));"
          " print(str(env(\"NOPE_XYZ_VAR\")));",
          "null\nnull\n");

    /* round is half-to-even, not half-away-from-zero. */
    check("round is half-to-even (banker's rounding)",
          "print(str(round(2.5))); print(str(round(3.5)));",
          "3\n4\n");

    /* abs/min/max go through double, so they always answer float. */
    check("abs/min/max answer float, and min/max are variadic",
          "print(str(abs(0 - 7))); print(str(min(3, 5, 1))); print(str(max(1, 9, 5)));",
          "7\n1\n9\n");

    check("strftime takes (format, timestamp)",
          "print(strftime(\"%Y\", 0));",
          "1970\n");

    /* html slots are {N} with N from 0; {{ }} are literal-brace escapes. */
    check("html fills {N} slots and folds {{ }} to one brace",
          "print(html(\"a{0}b{1}c\", \"X\", \"Y\")); print(html(\"{{x}}\"));"
          " print(html(\"a{5}b\", \"X\"));",
          "aXbYc\n{x}\na{5}b\n");

    /* html() marks its result trusted, so nesting it skips escaping — an
     * injection channel worth pinning so the SPEC warning stays true. */
    check("html() output is trusted and injects raw when nested",
          "print(html(\"<i>{0}</i>\", html(\"<b>&</b>\")));"
          " print(html(\"<i>{0}</i>\", \"<b>&</b>\"));",
          "<i><b>&</b></i>\n<i>&lt;b&gt;&amp;&lt;/b&gt;</i>\n");

    /* el escapes attribute values but validates neither tag nor prop name. */
    check("el escapes attribute values but not tag names",
          "print(render(el(\"a\", { href: \"x?a=1&b=2\" }, \"t\")));"
          " print(render(el(\"script\", {}, \"alert(1)\")));",
          "<a href=\"x?a=1&amp;b=2\">t</a>\n<script>alert(1)</script>\n");

    check_err("el() requires the props argument",
              "print(render(el(\"p\")));",
              "el() needs (tag, props, ...children)");

    check("string concat",
          "let a = \"hello\"; let b = \" world\"; print(a + b + \"!\");",
          "hello world!\n");

    check("replace global literal",
          "print(replace(\"a-b-c\", \"-\", \"+\"));\n"
          "print(replace(\"banana\", \"an\", \"AN\"));\n"
          "print(replace(\"hello\", \"x\", \"y\"));\n"
          "print(replace(\"abc\", \"\", \"X\"));\n"
          "print(replace(\"中文abc中文\", \"中文\", \"ZH\"));\n"
          "print(replace(\"aaaa\", \"aa\", \"b\"));\n",
          "a+b+c\nbANANa\nhello\nabc\nZHabcZH\nbb\n");

    check("adjacent string literals merge (JS-style)",
          "print(\"a\" \"b\" \"c\");\n"
          "let s = \"p\" \"q\";\n"
          "print(s);",
          "abc\npq\n");
    reject("adjacent non-string literal", "print(\"a\" 1);", "expected");

    check("comparisons",
          "print(str(2 < 3)); print(str(2 >= 3)); print(str(1 == 1)); "
          "print(str(1 != 2)); print(str(1 == 2));",
          "true\nfalse\ntrue\ntrue\nfalse\n");

    check("boolean logic",
          "print(str(true and false)); print(str(true or false)); "
          "print(str(not true)); print(str(not false));",
          "false\ntrue\nfalse\ntrue\n");

    /* 正向侧:! 的结果能绑进 bool 且值正确(仅靠 reject 只能证明「不该过的
     * 过不了」,还得有「该过的照旧过」这一半)。 */
    check("logical not keeps bool",
          "let b = true;\nlet x: bool = !b;\nlet y: bool = not b;\n"
          "print(str(x)); print(str(y));",
          "false\nfalse\n");

    check("let reassignment + if/else",
          "let x = 1;\nif (x == 1) { x = x + 10; } else { x = 0; }\n"
          "if (x > 5) { print(\"big\"); } else { print(\"small\"); }\n"
          "while (x < 15) { x = x + 1; }\nprint(str(x));",
          "big\n15\n");

    check("recursive function + return unwinding",
          "func fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
          "print(str(fact(6)));",
          "720\n");

    check("higher-order: function expression + map + keys/len/get",
          "let add = func(a, b) { return a + b; };\n"
          "let m = { first: 1, second: 2 };\n"
          "print(str(add(m.first, m.second)));\n"
          "print(str(len(keys(m))));\n"
          "print(str(get(m, \"first\")));\n"
          "print(str(get(m, \"nope\") == null));",
          "3\n2\n1\ntrue\n");

    check("arrow fn: assignment + call",
          "let f = (a) => { return a * 2; };\nprint(str(f(21)));",
          "42\n");

    check("arrow fn: immediate call in an expression",
          "print(str((x, y) => { return x + y; }(3, 4)));",
          "7\n");

    check("arrow fn: typed params",
          "let g = (n: int) => { return str(n) + \"!\"; };\nprint(g(5));",
          "5!\n");

    check("arrow fn: zero params",
          "let h = () => { return 99; };\nprint(str(h()));",
          "99\n");

    check("contextual keyword: get(m,k) builtin still callable",
          "let m = { a: 7 };\nprint(str(get(m, \"a\")));",
          "7\n");

    check("cast-and-get: int(m,k) is sugar for int(get(m,k))",
          "let m = { a: 2, b: \"9\" };\nprint(str(int(m, \"a\") + int(m, \"b\")));",
          "11\n");

    check("cast-and-get: missing key -> type zero / explicit default",
          "let m = { a: 2 };\nprint(str(int(m, \"z\")));\n"
          "print(str(int(m, \"z\", 7)));\nprint(str(str(m, \"z\")));\n"
          "print(str(m, \"z\", \"empty\"));",
          "0\n7\nnull\nempty\n");

    check("contextual keyword: get/post usable as map key + member",
          "let m = { get: 1, post: 2 };\nprint(str(m.get + m.post));",
          "3\n");

    check("method shorthand: get/post register routes",
          "get \"/p\", (req) => { return \"x\"; };\n"
          "post \"/p\", (req) => { return \"y\"; };\n"
          "print(\"registered\");",
          "registered\n");

    check("method shorthand: bare get(m,k) statement still an expression",
          "let m = { a: 1 };\nget(m, \"a\");\nprint(\"ok\");",
          "ok\n");

    check("verbs group: `w \"path\", h` registers one route per method",
          "verbs w = [\"POST\", \"PUT\"];\n"
          "w \"/p\", (req) => { return \"x\"; };\n"
          "print(\"registered\");",
          "registered\n");

    check("verbs group: alias is also an ordinary list variable",
          "verbs w = [\"POST\", \"PUT\"];\nprint(str(len(w)));\n"
          "print(get(w, 0));",
          "2\nPOST\n");

    check("verbs map: group maps methods to labels (req.label at dispatch)",
          "verbs w = { POST: \"created\", DELETE: \"removed\" };\n"
          "print(get(w, \"POST\"));\n"
          "w \"/p\", (req) => { return req.label; };\n"
          "print(\"registered\");",
          "created\nregistered\n");

    check("built-in write group: predeclared, maps methods to labels",
          "print(str(len(write)));\nprint(get(write, \"POST\"));\n"
          "write \"/p\", (req) => { return req.label; };\n"
          "print(\"registered\");",
          "4\ncreated\nregistered\n");

    check("built-in read group: predeclared GET/HEAD list",
          "print(str(len(read)));\nprint(get(read, 0));",
          "2\nGET\n");

    check("handler-less route: `write \"/p\";` registers with the default handler",
          "write \"/p\";\nread \"/q\";\nprint(\"registered\");",
          "registered\n");

    check("tool params: bare type keyword { a: int } registers",
          "tool \"t\", \"d\", { a: int }, (a) => { return { ok: 1 }; };\n"
          "print(\"registered\");",
          "registered\n");

    check("cast-and-get: float/bool/string callable casts",
          "let m = { f: \"2.5\", on: 1 };\n"
          "print(str(float(m, \"f\")));\nprint(str(bool(m, \"on\")));\nprint(string(42));",
          "2.5\ntrue\n42\n");

    check("list literal + indexing via get",
          "let xs = [10, 20, 30];\nprint(str(len(xs)));\n"
          "print(str(get(xs, 1)));",
          "3\n20\n");

    check("stringify round-trips strings/numbers/bools/null",
          "print(stringify({ a: \"x\", b: 1, c: true, d: null }));",
          "{\"a\":\"x\",\"b\":1,\"c\":true,\"d\":null}\n");

    check("json() parses tool-style input",
          "let m = json(\"{\\\"n\\\":3,\\\"s\\\":\\\"hi\\\"}\");\n"
          "print(str(m.n)); print(m.s);",
          "3\nhi\n");

    check("GC stress: 20k allocations in a loop",
          "func spam(n) { let t = \"\";\n"
          "  let i = 0; while (i < n) { let s = str(i) + \"x\"; t = t + s; i = i + 1; }\n"
          "  return len(t);\n}\nprint(str(spam(20000)));",
          "108890\n");

    check("nested maps + member assignment",
          "let m = { a: { b: 1 } };\nm.a.b = 99;\nprint(str(m.a.b));",
          "99\n");

    /* `.len` on a map that is bound to a named struct is the field the user
     * declared — that is what both native backends emit — so the interpreter
     * must not answer with the key count here. Regression: the OBJ_MAP branch
     * used to count unconditionally, and `pn.len` returned 2 while a compiled
     * run returned 7. */
    check("named struct: .len reads the declared field",
          "type P = { len: int, w: int };\n"
          "let pn: P = { len: 7, w: 3 };\n"
          "print(str(pn.len));",
          "7\n");

    /* The next two pin the other side of the same rule: only a map that
     * carries a static struct name reads a field. A plain anonymous map keeps
     * answering `.len` with its key count — including the awkward case where
     * one of those keys is literally called "len". */
    check("anonymous map: .len stays the key count",
          "let m = { a: 1, b: 2 };\n"
          "print(str(m.len));\n"
          "let k = { len: 5, a: 1 };\n"
          "print(str(k.len));",
          "2\n2\n");

    /* ---- Lume typed-language coverage ---- */

    check("inference: int stays int, float widens",
          "let i = 5;\nlet f = 5.5;\n"
          "print(str(i + i)); print(str(i + f)); print(str(f + f));",
          "10\n10.5\n11\n");

    check("typed struct decl + func returns struct",
          "type Pt = { x: int, y: int };\n"
          "func add_pt(a: Pt, b: Pt): Pt {\n"
          "  return { x: a.x + b.x, y: a.y + b.y };\n}\n"
          "let p: Pt = add_pt({ x: 1, y: 2 }, { x: 10, y: 20 });\n"
          "print(str(p.x + p.y));",
          "33\n");

    check("typed list",
          "let xs: int[] = [1, 2, 3];\n"
          "let total: int = get(xs, 0) + get(xs, 1) + get(xs, 2);\n"
          "print(str(total)); print(str(len(xs)));",
          "6\n3\n");

    check("Result ?: ok path unwraps",
          "func maybe(a: int): Result {\n"
          "  if (a > 0) { return { ok: a }; }\n"
          "  return { err: \"neg\" };\n}\n"
          "func use(): Result { let v = maybe(7)?; print(str(v)); return { ok: v }; }\n"
          "use();",
          "7\n");

    check("Result ?: err propagates up",
          "func maybe(a: int): Result {\n"
          "  if (a > 0) { return { ok: a }; }\n"
          "  return { err: \"neg\" };\n}\n"
          "func use(): Result { let v = maybe(-1)?; print(\"unreachable\"); return { ok: 1 }; }\n"
          "print(stringify(use()));",
          "{\"err\":\"neg\"}\n");

    check("explicit annotation honored by checker (int->int)",
          "let x: int = 1;\nprint(stringify(x));",
          "1\n");

    check("call args: trailing comma accepted (matches list/map)",
          "func f(a: int, b: int) { return a + b; }\n"
          "print(str(f(1, 2,))); print(str(len([1, 2, 3,])));",
          "3\n3\n");

    check("vdom: render() serializes el() trees (SSR)",
          "print(render(el(\"a\", { href: \"/x\" }, \"Home\")));\n"
          "print(render(el(\"section\", { data_page: \"home\", hidden: true }, \"Hi\")));",
          "<a href=\"/x\">Home</a>\n"
          "<section data-page=\"home\" hidden=\"true\">Hi</section>\n");

    check("vdom: escaping, void tags, on* dropped server-side",
          "print(render(el(\"img\", { src: \"/p.png\", onclick: \"nope\" })));\n"
          "print(render(el(\"p\", { class: false }, \"<script>x</script>\")));",
          "<img src=\"/p.png\">\n"
          "<p>&lt;script&gt;x&lt;/script&gt;</p>\n");

    check("vdom: props map expanded + children flattened",
          "print(render(el(\"div\", { class: \"btn\" }, "
          "el(\"b\", {}, \"x\"), \" and \", 42)));",
          "<div class=\"btn\"><b>x</b> and 42</div>\n");

    check("html template: positional slots, escaping, {{ }}",
          "print(html(\"<a class='{0}' href='{1}'>{2}</a>\", \"btn\", \"/x\", \"Go\"));\n"
          "print(html(\"<b>{0}</b>\", \"<script>x</script>\"));\n"
          "print(html(\"{{ {0} }}\", 7));",
          "<a class='btn' href='/x'>Go</a>\n"
          "<b>&lt;script&gt;x&lt;/script&gt;</b>\n"
          "{ 7 }\n");

    check("html template: vnode slots render structurally",
          "print(html(\"<main>{0}</main>\", el(\"big\", { id: \"c\" }, 7)));\n"
          "print(html(\"{0}\", \"raw text\"));",
          "<main><big id=\"c\">7</big></main>\n"
          "raw text\n");

    /* ---- discovery builtins (env/files/read_file/mcps/catalog) ---------- */

    check("env: set var -> string, unset -> null",
          "print(str(env(\"LUME_NO_SUCH_VAR_42\")));\n"
          "print(str(len(env(\"PATH\")) > 0));",
          "null\ntrue\n");

    /* Built before the check, and addressed by absolute /tmp path: the case
     * used to read the repo's `docs/` and only passed when the binary was
     * started from the checkout root. */
    if (smk_fs_fixture() == 0) {
        check("files: sorted listing + read_file success/missing path",
              "let fs = files(\"/tmp/lume-smoke-files\");\n"
              "let found = false;\n"
              "let i = 0;\n"
              "while (i < len(fs)) {\n"
              "  if (get(fs, i) == \"LUME.md\") { found = true; }\n"
              "  i = i + 1;\n"
              "}\n"
              "print(str(found));\n"
              "print(str(len(read_file(\"/tmp/lume-smoke-files/LUME.md\")) > 1000));\n"
              "print(str(read_file(\"/tmp/lume-smoke-files/no-such-file.md\") == null));",
              "true\ntrue\ntrue\n");
    }

    /* Missing sync file is "no MCP servers", which is an empty list, not null.
     * Publishing null here made /discovery return "mcps": null and the hub
     * pages threw on data.mcps.length. Real read failures (OOM, oversized,
     * malformed JSON) still yield null. */
    check("mcps: missing catalog -> empty list",
          "print(str(len(mcps()) == 0));",
          "true\n");

    /* Drop the catalog the sync would produce, then verify the builtin
     * parses it end-to-end (list of server maps). */
    {
        FILE *f = fopen(".data/mcp-servers-router.json", "wb");
        if (f) {
            fputs("[{\"id\":\"fs\",\"cmd\":\"mcp-pty\"}]", f);
            fclose(f);
        }
    }

    check("mcps: parses router catalog into a list",
          "let ms = mcps();\n"
          "print(str(len(ms) == 1));\n"
          "print(str(get(get(ms, 0), \"id\")));",
          "true\nfs\n");

    /* --- math builtins (src/builtins_math.c) ------------------------------
     * Integers print without a decimal point even though every result is a
     * C double (sqrt(16) -> "4", not "4.0"). docs/LUME.md pins that, and the
     * three domain guards below pin the runtime half of the contract. */
    check("math: abs/sqrt/pow",
          "print(str(abs(-3)));\n"
          "print(str(sqrt(16)));\n"
          "print(str(pow(2, 10)));",
          "3\n4\n1024\n");

    check("math: floor/ceil/round",
          "print(str(floor(1.7)));\n"
          "print(str(ceil(1.2)));\n"
          "print(str(round(2.5)));",
          "1\n2\n3\n");

    check("math: min/max take variadic numbers",
          "print(str(min(3, 1, 2)));\n"
          "print(str(max(3, 1, 2)));",
          "1\n3\n");

    check("math: exp/log/ln plus the pi constant",
          "print(str(exp(0)));\n"
          "print(str(log(1)));\n"
          "print(str(ln(exp(1))));\n"
          "print(str(pi));",
          "1\n0\n1\n3.14159\n");

    check_err("math: sqrt rejects a negative argument",
              "print(str(sqrt(-1)));", "sqrt(): negative argument");
    check_err("math: log rejects a non-positive argument",
              "print(str(log(0)));", "log(): argument must be positive");
    check_err("math: math builtins reject non-number arguments",
              "print(str(abs(\"3\")));", "expected number argument");
    check_err("math: min() requires at least one argument",
              "print(str(min()));", "min() expects at least one number");

    /* --- --no-fs runtime lock --------------------------------------------
     * Without these, a regression that quietly unguards read_file() would
     * still pass every other assertion, because the positive cases return
     * real data and nothing asserts the guard's refusal. Smoke-level tests
     * exercise the VM directly, which is what --no-fs configures. */
    check_err_nofs("no-fs: read_file refuses a path",
                   "print(str(read_file(\"/etc/hostname\")));",
                   "filesystem access is disabled");
    check_err_nofs("no-fs: write_file refuses a path",
                   "write_file(\"/tmp/lume-smoke-should-not-exist\", \"x\");",
                   "filesystem access is disabled");
    check_err_nofs("no-fs: mkdir refuses a path",
                   "mkdir(\"/tmp/lume-smoke-should-not-exist\");",
                   "filesystem access is disabled");
    check_err_nofs("no-fs: files refuses a directory",
                   "print(str(files(\"/tmp\")));",
                   "filesystem access is disabled");

    /* --- discovery builtins ---------------------------------------------- */
    check("now: returns a positive unix timestamp",
          "print(str(now() > 1600000000));", "true\n");

    check("push: appends in place and also returns the list",
          "let xs = [1, 2];\n"
          "print(str(len(xs)));\n"
          "print(str(push(xs, 3)));\n"
          "print(str(get(xs, 2)));",
          "2\n[1,2,3]\n3\n");

    check("catalog: names the three discovery tables",
          "print(str(keys(catalog())));",
          "[\"skills\",\"tools\",\"mcps\"]\n");

    check("discovery_endpoints: names the router endpoint table",
          "print(str(keys(discovery_endpoints())));",
          "[\"llm\",\"router\",\"model\"]\n");

/* The scratch-skill enumeration check is host-only: it needed the seeded
 * HARNESS_SKILLS_DIR table. skills() still exists here and returns an empty
 * list (no registry is seeded in this tree). */

    /* ---- product-tool builtins (invest.lume portfolio + report) ---- */

    check("strftime: localtime format of a timestamp",
          "print(strftime(\"%Y-%m-%d\", 0));"
          "print(strftime(\"%Y%m%d\", 0));",
          "1970-01-01\n19700101\n");

    check("put: dynamic-key map write + reference mutation",
          "let h = {};\n"
          "put(h, \"aapl\", { name: \"Apple\", units: 10, avg_cost: 150.5 });\n"
          "let cur = get(h, \"aapl\", null);\n"
          "cur.units = cur.units + 2;\n"
          "put(h, \"tsla\", { name: \"Tesla\", units: 5, avg_cost: 250.0 });\n"
          "print(str(len(keys(h)) == 2));\n"
          "print(str(get(h, \"aapl\", null).units));",
          "true\n12\n");

    /* The invest.lume ledger shape in miniature: weighted-average upsert +
     * remove via put(). Mirrors portfolio_add/portfolio_remove. */
    check("portfolio ledger: weighted-average upsert + remove",
          "let h = {};\n"
          "put(h, \"aapl\", { name: \"Apple\", units: 10, avg_cost: 100.0 });\n"
          "let cur = get(h, \"aapl\", null);\n"
          "let nu = cur.units + 10;\n"
          "cur.avg_cost = (cur.units * cur.avg_cost + 10 * 120.0) / nu;\n"
          "cur.units = nu;\n"
          "print(str(get(h, \"aapl\", null).units));\n"
          "print(str(get(h, \"aapl\", null).avg_cost));\n"
          "let out = {};\n"
          "let ks = keys(h);\n"
          "let i = 0;\n"
          "while (i < len(ks)) {\n"
          "  let k = get(ks, i);\n"
          "  if (k != \"aapl\") { put(out, k, get(h, k, null)); }\n"
          "  i = i + 1;\n"
          "}\n"
          "print(str(len(keys(out)) == 0));",
          "20\n110\ntrue\n");

    /* mkdir + write_file round-trip. write_file must read the inline string
     * payload — reading as.str.data (never set) was a latent crash the moment
     * fopen succeeded. */
    check("mkdir + write_file round-trip",
          "mkdir(\"/tmp/lume-smoke-data/a/b\");\n"
          "let ok = write_file(\"/tmp/lume-smoke-data/a/b/x.txt\", \"hibytes\");\n"
          "print(str(ok));\n"
          "print(read_file(\"/tmp/lume-smoke-data/a/b/x.txt\"));",
          "true\nhibytes\n");

    /* flock lock builtins: acquire, same-process re-acquire (replaces the old
     * lock), release, re-acquire. Cross-process mutual exclusion is exercised
     * by the runtime test suite (run_all.sh). */
    check("lock_file/unlock_file: acquire/replace/release cycle",
          "let a = lock_file(\"/tmp/lume-smoke-data/lock\", 100);\n"
          "let b = lock_file(\"/tmp/lume-smoke-data/lock\", 100);\n"
          "unlock_file();\n"
          "let c = lock_file(\"/tmp/lume-smoke-data/lock\", 100);\n"
          "unlock_file();\n"
          "print(str(a)); print(str(b)); print(str(c));",
          "true\ntrue\ntrue\n");

    /* ---- for / break / continue ---- */

    check("for-in over range",
          "let s = 0;\nfor (x in range(5)) { s = s + x; }\nprint(s);", "10\n");
    check("C-style for with let init",
          "let s = 0;\nfor (let i = 0; i < 5; i = i + 1) { s = s + i; }\nprint(s);", "10\n");
    check("C-style for with expr init",
          "let i = 0;\nlet s = 0;\nfor (i = 0; i < 3; i = i + 1) { s = s + 1; }\nprint(s);", "3\n");
    check("for-in with let",
          "let s = \"\";\nfor (let x in range(3)) { s = s + str(x); }\nprint(s);", "012\n");
    check("for-in over map keys",
          "let m = { a: 1, b: 2 };\nlet k = \"\";\nfor (x in m) { k = k + x; }\nprint(k);", "ab\n");
    check("break in for",
          "let c = 0;\nfor (let i = 0; i < 100; i = i + 1) { if (i == 3) { break; } c = c + 1; }\nprint(c);", "3\n");
    check("continue in for",
          "let c = 0;\nfor (let i = 0; i < 5; i = i + 1) { if (i == 2) { continue; } c = c + 1; }\nprint(c);", "4\n");
    check("break in while",
          "let w = 0;\nwhile (true) { w = w + 1; if (w > 3) { break; } }\nprint(w);", "4\n");
    check("continue in while",
          "let o = 0;\nlet i = 0;\nwhile (i < 6) { i = i + 1; if (i % 2 == 0) { continue; } o = o + 1; }\nprint(o);", "3\n");
    check("nested for, inner break scoped",
          "let p = 0;\nfor (a in range(3)) { for (b in range(3)) { if (b == 1) { break; } p = p + 1; } }\nprint(p);", "3\n");

    /* ---- collection tools ---- */

    check("range stop", "print(len(range(4)));", "4\n");
    check("range start-stop", "print(get(range(1, 6), 0));", "1\n");
    check("range step", "print(get(range(0, 10, 3), 3));", "9\n");
    check("map named func",
          "func dbl(x) { return x * 2; }\nprint(get(map(dbl, range(1, 6)), 4));", "10\n");
    check("map lambda",
          "print(get(map(func (x) { return x * 3; }, range(1, 6)), 2));", "9\n");
    check("filter",
          "func even(x) { return x % 2 == 0; }\nprint(len(filter(even, range(1, 6))));", "2\n");
    check("reduce",
          "func add(a, b) { return a + b; }\nprint(reduce(add, range(1, 6), 0));", "15\n");

    /* ---- type checker rejects ---- */

    reject("let type mismatch", "let x: int = \"hi\";", "assignable");
    /* 一元 not 的静态类型曾经漏报:`!b` 的 case 只有 is_bool_ok()、既没 return
     * 也没 break,于是继续落进下一个 case N_BINARY。两个结构体共用 union 布局
     * (binary.op == unary.op、binary.left == unary.operand),binary.right 则
     * 读到不相关的槽(实际为 NULL),arith_result(operand, NULL) 直接短路成
     * any_type()。后果是 `!b` 的静态类型变 any,而赋值检查遇到 any 无条件放行
     * ——`let i: int = !b;` 编译全绿,只有运行时才暴露。下面三条把「! 的结果是
     * bool 而不是它操作数的类型」钉死(见 docs/PITFALLS.md 的 switch 漏return 一条)。 */
    reject("! is bool, not int",
           "let b = true;\nlet x: int = !b;", "assignable");
    reject("not is bool, not string",
           "let b = true;\nlet s: string = not b;", "assignable");
    reject("! on a non-bool operand",
           "print(str(!1));", "expected bool");
    reject("strict bool in if",
          "let x = 1;\nif (x) { print(\"y\"); }", "expected bool");
    reject("strict bool in and",
          "let x = 1;\nprint(str(true and x));", "expected bool");
    reject("unknown type name", "type A = { v: Nope };", "unknown type");
    reject("? outside a function",
          "func f(): Result { return { err: \"e\" }; }\nlet r = f()?;",
          "inside a function");
    reject("? on a non-Result call",
          "func f(): int { return 1; }\n"
          "func g(): Result { let x = f()?; return { ok: 1 }; }",
          "does not return Result");
    reject("undefined variable in expr", "print(x);", "undefined variable");
    reject("missing struct field",
          "type P = { x: int, y: int };\nfunc p(): P { return { x: 1 }; }",
          "missing field");
    reject("call arity mismatch", "func f(a: int) { }\nf(1, 2);", "arguments");
    reject("map key not in struct",
          "type P = { x: int };\nfunc p(): P { return { x: 1, z: 2 }; }",
          "no field");
    reject("break outside loop", "break;", "outside a loop");
    reject("continue outside loop", "continue;", "outside a loop");
    reject("break in func body", "func f() { break; }", "outside a loop");


    /* ---- multi-file modules (import/export) ---- */
    {
        static const char *f[][2] = {
            {"lib.lume",
             "export let TAX_RATE = 0.13;\n"
             "let hidden = \"module-private\";\n"
             "export func tax(amount) { return amount * TAX_RATE; }\n"},
            {"main.lume",
             "import \"lib.lume\" as lib;\n"
             "print(\"tax =\", lib.tax(1000));\n"
             "print(\"rate =\", lib.TAX_RATE);\n"
             "print(\"sum =\", lib.tax(1000) + lib.TAX_RATE);\n"},
        };
        check_modules("modules import + namespace call", f, 2,
                      "main.lume", "tax = 130\nrate = 0.13\nsum = 130.13\n",
                      NULL);
    }
    {
        static const char *f[][2] = {
            {"a.lume",
             "import \"b.lume\" as b;\nprint(b.x);\n"},
            {"b.lume",
             "import \"a.lume\" as a;\nexport let x = 1;\n"},
        };
        check_modules("modules circular import rejected", f, 2, "a.lume",
                      "", "circular import");
    }
    {
        static const char *f[][2] = {
            {"c.lume",
             "print(\"C top-level runs\");\nexport let V = 7;\n"},
            {"b.lume",
             "import \"c.lume\" as c;\n"
             "export func from_b() { return c.V + 1; }\n"},
            {"a.lume",
             "import \"c.lume\" as c;\n"
             "import \"b.lume\" as b;\n"
             "print(\"V=\", c.V, \" b=\", b.from_b());\n"},
        };
        check_modules("modules diamond runs dep once", f, 3, "a.lume",
                      "C top-level runs\nV= 7  b= 8\n", NULL);
    }
    {
        static const char *f[][2] = {
            {"lib.lume",
             "export func seven() { return 7; }\nlet secret = 1;\n"},
            {"main.lume",
             "import \"lib.lume\" as lib;\nprint(lib.secret);\n"},
        };
        check_modules("modules unexported member rejected", f, 2, "main.lume",
                      "", "no export 'secret'");
    }
    {
        static const char *f[][2] = {
            {"lib.lume",
             "export func seven() { return 7; }\n"},
            {"main.lume",
             "import \"lib.lume\" as lib;\n"
             "import \"lib.lume\" as lib;\n"},
        };
        check_modules("modules duplicate namespace rejected", f, 2,
                      "main.lume", "", "duplicate name 'lib'");
    }
    {
        static const char *f[][2] = {
            {"main.lume",
             "import \"missing.lume\" as m;\nprint(m.x);\n"},
        };
        check_modules("modules missing file rejected", f, 1, "main.lume", "",
                      "cannot resolve import");
    }

    /* --- http_get:离线可判定的两个闸门 (2026-10-04) --- */
    {
        /* 总闸:--no-net / LUME_NO_NET=1 时 http_get 一个字都不发。 */
        check_err_nonet(
            "http_get no-net 总闸拒绝",
            "let r = http_get(\"http://example.com/\", { timeout: 3 });\n",
            "network access is disabled");
        /* 总闸同样压住私有地址(先查锁再查闸门)。 */
        check_err_nonet(
            "http_get no-net 优先于 SSRF 闸门",
            "let r = http_get(\"http://127.0.0.1/\", { timeout: 3 });\n",
            "network access is disabled");
    }
    {
        /* SSRF 闸门:socket 都没开就拒绝,所以 CI 无网也能跑。 */
        check_http_offline(
            "http_get 拒绝回环地址",
            "let r = http_get(\"http://127.0.0.1/\", { timeout: 3 });\n"
            "print(\"ok=\", get(r, \"ok\", false));\n"
            "print(\"err=\", str(get(r, \"err\", \"\")));\n",
            "ok= false\n"
            "err= http_get(): refused — 127.0.0.1 resolves to a private/reserved "
            "address\n");
    }
    {
        /* 云元数据地址:这条最要紧,打了就可能被当成内网探测。 */
        check_http_offline(
            "http_get 拒绝云元数据地址",
            "let r = http_get(\"http://169.254.169.254/latest/meta-data/\", {\n"
            "  timeout: 3 });\n"
            "print(\"ok=\", get(r, \"ok\", false));\n",
            "ok= false\n");
    }

    /* --- http_post/put/patch/delete(2026-10-05):五个谓词必须走同一套闸门 ---
     * 复制实现最容易漏掉的正是 no_net 与 host_blocked 这两处检查 —— 漏一个
     * 就等于给出站通道开个新口子。所以这里按动词表逐条跑过去,谁漏了谁红。 */
    {
        static const char *verbs[] = { "http_get", "http_post", "http_put",
                                       "http_patch", "http_delete" };
        const int nverbs = (int)(sizeof verbs / sizeof verbs[0]);
        for (int i = 0; i < nverbs; i++) {
            char name[64], src[192];
            snprintf(name, sizeof name, "%s no-net 总闸拒绝", verbs[i]);
            snprintf(src, sizeof src,
                     "let r = %s(\"http://example.com/\", { timeout: 3 });\n",
                     verbs[i]);
            check_err_nonet(name, src, "network access is disabled");
        }
    }
    {
        static const char *verbs[] = { "http_get", "http_post", "http_put",
                                       "http_patch", "http_delete" };
        const int nverbs = (int)(sizeof verbs / sizeof verbs[0]);
        for (int i = 0; i < nverbs; i++) {
            char name[96], src[256], want[256];
            snprintf(name, sizeof name, "%s 拒绝回环地址", verbs[i]);
            snprintf(src, sizeof src,
                     "let r = %s(\"http://127.0.0.1/\", { timeout: 3 });\n"
                     "print(\"ok=\", get(r, \"ok\", false));\n"
                     "print(\"err=\", str(get(r, \"err\", \"\")));\n", verbs[i]);
            /* 错误前缀跟着动词走(内建要报自己那个名字),其余文案完全一致。 */
            snprintf(want, sizeof want,
                     "ok= false\n"
                     "err= %s(): refused — 127.0.0.1 resolves to a private/reserved "
                     "address\n", verbs[i]);
            check_http_offline(name, src, want);
        }
    }

    /* --- http_* 的 URL 解析分支 (2026-10-05) ---
     * 这几条在 url 解析阶段就返回,一个 socket 都没开,所以和上面那两条闸门
     * 一样是离线可判定的。锁两件事:畸形输入绝不许走到 connect;五个谓词共用
     * 同一段解析(谁哪天复制了一份自己的解析,这里立刻红)。 */
    {
        static const char *verbs[] = { "http_get", "http_post", "http_put",
                                       "http_patch", "http_delete" };
        const int nverbs = (int)(sizeof verbs / sizeof verbs[0]);
        for (int i = 0; i < nverbs; i++) {
            char name[96], src[256], want[256];
            snprintf(name, sizeof name, "%s 拒绝无 scheme 的 URL", verbs[i]);
            snprintf(src, sizeof src,
                     "let r = %s(\"not a url\", {});\n"
                     "print(\"ok=\", get(r, \"ok\", false));\n"
                     "print(\"err=\", str(get(r, \"err\", \"\")));\n", verbs[i]);
            snprintf(want, sizeof want,
                     "ok= false\n"
                     "err= %s(): unsupported url: not a url\n", verbs[i]);
            check_http_offline(name, src, want);
        }
    }
    {
        check_http_offline(
            "http_get 拒绝非 http/https 协议",
            "let r = http_get(\"ftp://example.com/x\", {});\n"
            "print(\"ok=\", get(r, \"ok\", false));\n"
            "print(\"err=\", str(get(r, \"err\", \"\")));\n",
            "ok= false\n"
            "err= http_get(): unsupported url: ftp://example.com/x\n");
        check_http_offline(
            "http_get 拒绝空 host",
            "let r = http_get(\"http://\", {});\n"
            "print(\"ok=\", get(r, \"ok\", false));\n"
            "print(\"err=\", str(get(r, \"err\", \"\")));\n",
            "ok= false\n"
            "err= http_get(): unsupported url: http://\n");
        /* 空串这条只断言 ok:上面几条的错误信息都停在非空字符上,空串那条末尾
         * 是个空格,把尾随空白钉进字符串字面量,之后任何一次 reformat 都能把
         * 这条用例悄悄改红。这条要锁的是「不崩、不开 socket、ok=false」。 */
        check_http_offline(
            "http_get 拒绝空串 URL",
            "let r = http_get(\"\", {});\n"
            "print(\"ok=\", get(r, \"ok\", false));\n",
            "ok= false\n");
    }

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}