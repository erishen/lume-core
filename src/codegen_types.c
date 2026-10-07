/* codegen_types.c — LLVM type spellings for the text backend.

 * One job: lume's `Type` has to become the string a `getelementptr` or an
 * `alloca` prints. Two spellings per named struct (`%Rect` for the aggregate
 * and `%Rect*` for a pointer to one), interned in the table at the top of this
 * file, and one for every scalar.
 *
 * Kept apart from the emitters because every section below asks for it and
 * none of them wants a copy of the spelling table.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "lume.h"
#include "irbuf.h"
#include "codegen_internal.h"

/* ----------------------------------------------------------- type mapping --- */

/* LLVM spellings of named struct types carry the `%` sigil. They are interned
 * rather than written into a scratch buffer, because llvm_type_of is called
 * twice inside a single format string (`getelementptr inbounds %s, %s* %s`),
 * and a shared scratch buffer would be overwritten before the format ran. */
#define MAX_STRUCT_SPELLINGS 64
typedef struct { char name[160]; char val[160]; char ptr[160]; } StructSpell;
static StructSpell struct_spell[MAX_STRUCT_SPELLINGS];
static int  struct_spell_n;

/* Two spellings per struct: `%Rect` for the aggregate itself (alloca/store) and
 * `%Rect*` for a reference to one (parameters, arguments, variable reads). */
/* forward: same tu, defined below this point. */
static const char *intern_struct_spelling(const char *name, int as_ptr);

/* Two spellings per struct: `%Rect` for the aggregate itself (alloca/store) and
 * `%Rect*` for a reference to one (parameters, arguments, variable reads). */

static const char *intern_struct_spelling(const char *name, int as_ptr)
{
    for (int i = 0; i < struct_spell_n; i++)
        if (strcmp(struct_spell[i].name, name) == 0)
            return as_ptr ? struct_spell[i].ptr : struct_spell[i].val;

    if (struct_spell_n >= MAX_STRUCT_SPELLINGS) return NULL;
    StructSpell *s = &struct_spell[struct_spell_n++];
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->val,  sizeof s->val,  "%%%s",  name);
    snprintf(s->ptr,  sizeof s->ptr,  "%%%s*", name);
    return as_ptr ? s->ptr : s->val;
}

/* How a reference to `t` is spelled: same as llvm_type_of, except that a struct
 * is always referenced through a pointer — that is how struct parameters and
 * arguments are declared. */

const char *llvm_ptr_type_of(Type *t)
{
    if (t && t->kind == TY_STRUCT) return intern_struct_spelling(t->name, 1);
    return llvm_type_of(t);
}

/* lume Type -> LLVM type spelling. NULL means "not supported by this backend". */

const char *llvm_type_of(Type *t)
{
    if (!t) return NULL;
    switch (t->kind) {
    case TY_INT:    return "i64";
    case TY_FLOAT:  return "double";
    case TY_BOOL:   return "i1";
    case TY_STRING: return "i8*";
    case TY_NULL:   return "i8*";   /* opaque null pointer; rt.c prints it */
    case TY_STRUCT:
        /* An *anonymous* struct is a map literal (a heap map keyed by strings),
         * which travels exactly like a list: one opaque pointer. A *named*
         * one is the aggregate case below. */
        if (!t->name) return "i8*";
        return intern_struct_spelling(t->name, 0);
    /* A list or a map lives on the heap (src/rt.c), so a value of this type
     * travels as the pointer itself — exactly like a string. */
    case TY_LIST:   return "i8*";
    default:        return NULL;
    }
}
