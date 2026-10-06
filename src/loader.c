/* Module loader for Lume — multi-file import/export (plan B).
 *
 * One source file = one module, with its own top-level Env and its own export
 * table. The entry script and every module it transitively imports are:
 *   1. parsed,
 *   2. type-checked (dependencies first, so an importer can resolve
 *      `ns.member` against the dependency's export signatures),
 *   3. — unless --check — executed top-level first (dependencies first, each
 *      module exactly once), leaving the entry module's top-level last and its
 *      Env bound as vm->globals.
 *
 * Import resolution is relative to the importing file's directory; paths are
 * canonicalized with realpath(3), which also collapses `a/../b` variants so
 * the module cache keys on one spelling. A module already on the load stack is
 * a cycle — reported as an error. Runtime binding: each N_IMPORT node's path
 * is rewritten to the canonical absolute path here, and the interpreter binds
 * `import "x" as ns` to the dependency's exports Env at statement time.
 */

#include "lume.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <stdlib.h>   /* _fullpath */
#endif

/* ---------- file io ---------- */

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n && n != 0) {
        fclose(f); free(buf); return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    if (len_out) *len_out = (size_t)n;
    return buf;
}

/* Unescape the quote-inclusive string token body into a plain C string.
 * Supports the lexer's escape set: \n \t \r \\ \" \0. Returns malloc'd or NULL
 * on a malformed body (no closing quote). */
static char *unescape_path(const char *q, size_t qlen) {
    if (qlen < 2 || q[0] != '"' || q[qlen - 1] != '"') return NULL;
    const char *p = q + 1;
    const char *end = q + qlen - 1;
    char *out = malloc(qlen);
    if (!out) return NULL;
    size_t o = 0;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            switch (p[1]) {
                case 'n': out[o++] = '\n'; p += 2; continue;
                case 't': out[o++] = '\t'; p += 2; continue;
                case 'r': out[o++] = '\r'; p += 2; continue;
                case '0': out[o++] = '\0'; p += 2; continue;
                case '\\': out[o++] = '\\'; p += 2; continue;
                case '"':  out[o++] = '"';  p += 2; continue;
                default:   out[o++] = *p++; continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

/* ---------- registry ---------- */

Module *loader_find(VM *vm, const char *canon_path) {
    for (int i = 0; i < vm->module_count; i++)
        if (strcmp(vm->modules[i]->path, canon_path) == 0)
            return vm->modules[i];
    return NULL;
}

static int loader_register(VM *vm, Module *m) {
    vm->modules = realloc(vm->modules,
                          sizeof(Module *) * ((size_t)vm->module_count + 1));
    if (!vm->modules) return -1;
    vm->modules[vm->module_count++] = m;
    return 0;
}

static int push_stack(VM *vm, const char *path) {
    vm->load_stack = realloc(vm->load_stack,
                             sizeof(vm->load_stack[0]) * ((size_t)vm->load_depth + 1));
    if (!vm->load_stack) return -1;
    vm->load_stack[vm->load_depth++] = path;
    return 0;
}

static void pop_stack(VM *vm) { vm->load_depth--; }

static bool on_stack(VM *vm, const char *path) {
    for (int i = 0; i < vm->load_depth; i++)
        if (strcmp(vm->load_stack[i], path) == 0) return true;
    return false;
}

/* ---------- path resolution ---------- */

/* dirname of a canonical absolute path (malloc'd). */
static char *path_dirname(const char *path) {
    /* Accept BOTH '/' (POSIX, and the joined paths we build with snprintf)
     * and '\' — because on Windows _fullpath() returns a backslash-separated
     * canonical path. A naive strrchr(path, '/') returned "." on Windows,
     * which made every relative import resolve against the CWD instead of
     * the importing file's directory, so `import "tax.lume"` from
     * examples/modules/app.lume looked in the repo root and failed with
     * "cannot read <root>/tax.lume". */
    const char *sep = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') sep = p;
    if (!sep) return strdup(".");
    if (sep == path) return strdup("/");
    size_t n = (size_t)(sep - path);
    char *dir = malloc(n + 1);
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

/* Resolve a raw import path (relative to `from_dir`) to a canonical absolute
 * path. NULL + message on failure. */
static char *resolve_import(const char *from_dir, const char *rel,
                            char *errbuf, size_t errbuf_size) {
    char joined[PATH_MAX];
    /* An import is absolute if it starts with '/', or (on Windows) with
     * '\' or a drive letter "X:". Without the Windows branch a "C:\..."
     * import would be treated as relative and joined onto from_dir. */
    int abs = (rel[0] == '/')
#ifdef _WIN32
        || (rel[0] == '\\')
        || (((rel[0] >= 'A' && rel[0] <= 'Z') || (rel[0] >= 'a' && rel[0] <= 'z'))
            && rel[1] == ':')
#endif
        ;
    if (abs) {
        snprintf(joined, sizeof(joined), "%s", rel);
    } else {
        snprintf(joined, sizeof(joined), "%s/%s", from_dir, rel);
    }
    char canon[PATH_MAX];
#ifdef _WIN32
    if (!_fullpath(canon, joined, sizeof(canon))) {
#else
    if (!realpath(joined, canon)) {
#endif
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "cannot resolve import '%s': %s",
                     rel, strerror(errno));
        return NULL;
    }
    return strdup(canon);
}

/* ---------- module loading ---------- */

static int load_module(VM *vm, const char *path, char *errbuf, size_t errbuf_size);

/* Collect N_IMPORT statements from a freshly parsed program into the module,
 * rewriting each node's path to its canonical absolute path. */
static int collect_imports(VM *vm, Module *m, char *errbuf, size_t errbuf_size) {
    (void)vm; /* the loader rewrites import paths before any VM runs */
    char *dir = path_dirname(m->path);
    if (!dir) return -1;
    for (int i = 0; i < m->prog->as.program.count; i++) {
        Node *s = m->prog->as.program.stmts[i];
        if (s->type != N_IMPORT) continue;
        if (s->is_export) {
            snprintf(errbuf, errbuf_size, "line %zu: cannot 'export' an import",
                     s->line);
            free(dir);
            return -1;
        }
        char *raw = unescape_path(s->as.imp.path, strlen(s->as.imp.path));
        if (!raw) {
            snprintf(errbuf, errbuf_size, "line %zu: malformed import path",
                     s->line);
            free(dir);
            return -1;
        }
        char *canon = resolve_import(dir, raw, errbuf, errbuf_size);
        free(raw);
        if (!canon) { free(dir); return -1; }
        /* rewrite the node so the interpreter can find the module */
        free(s->as.imp.path);
        s->as.imp.path = canon;
        m->import_paths = realloc(m->import_paths,
                                  sizeof(char *) * ((size_t)m->import_count + 1));
        m->ns_names = realloc(m->ns_names,
                              sizeof(char *) * ((size_t)m->import_count + 1));
        if (!m->import_paths || !m->ns_names) { free(dir); return -1; }
        m->import_paths[m->import_count] = canon;
        /* Borrowed, not strdup'd: node_free() releases imp.ns together with
         * the rest of the AST, and loader_free() only frees the two vector
         * shells. Copying here leaked one string per imported module. */
        m->ns_names[m->import_count] = s->as.imp.ns;
        m->import_count++;
    }
    free(dir);
    return 0;
}

static Module *module_new(const char *path) {
    Module *m = calloc(1, sizeof(Module));
    if (!m) return NULL;
    m->path = strdup(path);
    return m;
}

static int load_module(VM *vm, const char *path, char *errbuf, size_t errbuf_size) {
    Module *found = loader_find(vm, path);
    if (found) {
        if (found->loading) {
            snprintf(errbuf, errbuf_size,
                     "circular import: '%s' imports itself (directly or "
                     "indirectly)", path);
            return -1;
        }
        return 0; /* already loaded (or load already in flight below) */
    }

    if (on_stack(vm, path)) {
        snprintf(errbuf, errbuf_size,
                 "circular import involving '%s'", path);
        return -1;
    }

    Module *m = module_new(path);
    if (!m) { snprintf(errbuf, errbuf_size, "out of memory"); return -1; }

    size_t len = 0;
    char *source = read_file(path, &len);
    if (!source) {
        snprintf(errbuf, errbuf_size, "cannot read %s", path);
        free(m->path);
        free(m);
        return -1;
    }
    char perr[512] = {0};
    m->prog = parse_program(source, perr, sizeof(perr));
    free(source);
    if (!m->prog) {
        snprintf(errbuf, errbuf_size, "%s (%s)", perr[0] ? perr : "parse error",
                 path);
        free(m->path);
        free(m);
        return -1;
    }

    if (loader_register(vm, m) != 0) {
        snprintf(errbuf, errbuf_size, "out of memory");
        free(m->path);
        free(m->prog);
        free(m);
        return -1;
    }

    m->loading = true;
    if (push_stack(vm, m->path) != 0) {
        snprintf(errbuf, errbuf_size, "out of memory");
        return -1;
    }

    if (collect_imports(vm, m, errbuf, errbuf_size) != 0) return -1;

    /* depth-first: dependencies load + type-check first */
    for (int i = 0; i < m->import_count; i++) {
        if (load_module(vm, m->import_paths[i], errbuf, errbuf_size) != 0)
            return -1;
    }

    pop_stack(vm);
    m->loading = false;

    /* type-check this module now that every dependency's export signatures
     * are known (type_check_module resolves imports through vm->modules) */
    if (!type_check_module(m, vm->modules, vm->module_count,
                           m->prog, errbuf, errbuf_size))
        return -1;
    m->typechecked = true;
    return 0;
}

/* ---------- execution (topological, once each) ---------- */

/* forward from interp.c */
void exec_module_top(VM *vm, Module *m);

static int exec_module(VM *vm, Module *m, char *errbuf, size_t errbuf_size) {
    for (int i = 0; i < m->import_count; i++) {
        Module *dep = loader_find(vm, m->import_paths[i]);
        if (!dep) {
            snprintf(errbuf, errbuf_size, "internal: module '%s' not loaded",
                     m->import_paths[i]);
            return -1;
        }
        if (exec_module(vm, dep, errbuf, errbuf_size) != 0) return -1;
    }
    if (m->executed) return 0;
    if (!m->env) {
        /* dependency module: fresh top-level Env (parent = entry globals, so
         * builtins stay visible), rooted for the process lifetime via the
         * GC's active-env chain */
        m->env = (Env *)AS_OBJ(make_env(vm, vm->globals));
        m->env->next_active = vm->active_envs;
        vm->active_envs = m->env;
    }
    if (!m->exports) {
        m->exports = (Env *)AS_OBJ(make_env(vm, NULL));
        m->exports->next_active = vm->active_envs;
        vm->active_envs = m->exports;
    }
    exec_module_top(vm, m);
    if (vm->error) {
        snprintf(errbuf, errbuf_size, "%s", vm->error_msg);
        return -1;
    }
    m->executed = true;
    return 0;
}

/* ---------- teardown ---------- */

void loader_free(VM *vm) {
    for (int i = 0; i < vm->module_count; i++) {
        Module *m = vm->modules[i];
        if (!m) continue;
        node_free(m->prog); /* the AST and every string inside it */
        free(m->path);
        /* import_paths[i] and the N_IMPORT node's `path` are the same block —
         * collect_imports() stores canon in both places — and ns_names[i]
         * shares the node's `ns`, so only the two vector *shells* are freed
         * here; the strings themselves die with the node. */
        free(m->import_paths);
        free(m->ns_names);
        for (int j = 0; j < m->export_types.count; j++)
            free(m->export_types.names[j]);
        free(m->export_types.names);
        free(m->export_types.types);
        free(m);
    }
    free(vm->modules);
    vm->modules = NULL;
    vm->module_count = 0;
    vm->export_env = NULL;
}

/* ---------- public entry ---------- */

int loader_run(VM *vm, const char *entry_path, bool check_only,
               char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    vm->modules = NULL;
    vm->module_count = 0;
    vm->load_stack = NULL;
    vm->load_depth = 0;

    char canon[PATH_MAX];
#ifdef _WIN32
    if (!_fullpath(canon, entry_path, sizeof(canon))) {
#else
    if (!realpath(entry_path, canon)) {
#endif
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "cannot resolve %s: %s", entry_path,
                     strerror(errno));
        return 1;
    }

    if (load_module(vm, canon, errbuf, errbuf_size) != 0) return 1;
    if (check_only) return 0;

    Module *entry = loader_find(vm, canon);
    /* the entry module's top level IS the program: reuse vm->globals so the
     * builtins seeded by bridge_seed_builtins stay visible */
    entry->env = vm->globals;
    entry->is_entry = true;   /* only its `main()` call sets the exit status */
    if (exec_module(vm, entry, errbuf, errbuf_size) != 0) return 1;
    return 0;
}
