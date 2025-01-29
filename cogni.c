#define _FORTIFY_SOURCE 3
#include "cogni.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <inttypes.h>
#include <execinfo.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <math.h>
#include <wchar.h>
#include <locale.h>

#ifndef cog_malloc
#define cog_malloc malloc
#endif
#ifndef cog_free
#define cog_free free
#endif

#define min(a, b) ((a) < (b) ? (a) : (b))

#define trace() printf("TRACE: %s: reached %s:%i\n", __func__, __FILE__, __LINE__)
static void print_backtrace();
static void debug_dump_stuff();

// MARK: GLOBALS

typedef struct chunk chunk;

static struct {
    chunk* mem;
    cog_object* freelist;
    size_t freespace;
    size_t alloc_chunks;
    cog_object* gc_protected;

    cog_object* stdout_stream;
    cog_object* stdin_stream;
    cog_object* stderr_stream;

    cog_object* modules;

    cog_object* stack;
    cog_object* command_queue;
    cog_object* scopes;

    cog_object* error_sym;
    cog_object* not_impl_sym;
    cog_object* on_exit_sym;
    cog_object* on_enter_sym;
} COG_GLOBALS = {0};

cog_object* cog_not_implemented() {
    return COG_GLOBALS.not_impl_sym;
}

cog_object* cog_error() {
    return COG_GLOBALS.error_sym;
}

cog_object* cog_on_exit() {
    return COG_GLOBALS.on_exit_sym;
}

cog_object* cog_on_enter() {
    return COG_GLOBALS.on_enter_sym;
}

cog_object* cog_get_stdout() {
    return COG_GLOBALS.stdout_stream;
}

cog_object* cog_get_stderr() {
    return COG_GLOBALS.stderr_stream;
}

cog_object* cog_get_stdin() {
    return COG_GLOBALS.stdin_stream;
}

void cog_set_stdout(cog_object* stream) {
    COG_GLOBALS.stdout_stream = stream;
}

void cog_set_stderr(cog_object* stream) {
    COG_GLOBALS.stderr_stream = stream;
}

void cog_set_stdin(cog_object* stream) {
    COG_GLOBALS.stdin_stream = stream;
}

// MARK: GC

#define COG_MEM_CHUNK_SIZE 32
struct chunk {
    cog_object mem[COG_MEM_CHUNK_SIZE];
    chunk* next;
};

cog_object* cog_make_obj(cog_obj_type* type) {
    if (COG_GLOBALS.freespace == 0) {
        chunk* newchunk = (chunk*)cog_malloc(sizeof(chunk));
        if (newchunk == NULL) {
            perror(__func__);
            abort();
        }
        memset(newchunk, 0, sizeof(*newchunk));
        newchunk->next = COG_GLOBALS.mem;
        COG_GLOBALS.mem = newchunk;
        for (size_t i = 0; i < COG_MEM_CHUNK_SIZE; i++) {
            newchunk->mem[i].next = COG_GLOBALS.freelist;
            COG_GLOBALS.freelist = &newchunk->mem[i];
            COG_GLOBALS.freespace++;
        }
        COG_GLOBALS.alloc_chunks++;
    }
    cog_object* obj = COG_GLOBALS.freelist;
    COG_GLOBALS.freelist = obj->next;
    obj->next = NULL;
    obj->type = type;
    COG_GLOBALS.freespace--;
    return obj;
}

void cog_make_immortal(cog_object* obj) {
    cog_push_to(&COG_GLOBALS.gc_protected, obj);
}

void cog_walk(cog_object* root, cog_walk_fun callback, cog_object* cookie) {
    walk:
    if (root == NULL) return;
    if (callback(root, cookie)) {
        if (root->type == NULL) {
            cog_walk(root->data, callback, cookie);
            root = root->next;
            goto walk;
        }
        if (root->type->walk) {
            root = root->type->walk(root, callback, cookie);
            goto walk;
        }
    }
}

cog_object* cog_walk_only_next(cog_object* obj, cog_walk_fun f, cog_object* arg) {
    (void)arg, (void)f;
    return obj->next;
}

cog_object* cog_walk_both(cog_object* obj, cog_walk_fun f, cog_object* arg) {
    cog_walk(obj->data, f, arg);
    return obj->next;
}

static bool markobject(cog_object* obj, cog_object* arg) {
    (void)arg;
    if (obj->marked) return false;
    obj->marked = true;
    return true;
}

static void gc() {
    cog_walk(COG_GLOBALS.gc_protected, markobject, NULL);
    cog_walk(COG_GLOBALS.stdout_stream, markobject, NULL);
    cog_walk(COG_GLOBALS.stdin_stream, markobject, NULL);
    cog_walk(COG_GLOBALS.stderr_stream, markobject, NULL);
    cog_walk(COG_GLOBALS.modules, markobject, NULL);
    cog_walk(COG_GLOBALS.stack, markobject, NULL);
    cog_walk(COG_GLOBALS.command_queue, markobject, NULL);
    cog_walk(COG_GLOBALS.scopes, markobject, NULL);
    cog_walk(COG_GLOBALS.error_sym, markobject, NULL);
    cog_walk(COG_GLOBALS.not_impl_sym, markobject, NULL);
    cog_walk(COG_GLOBALS.on_exit_sym, markobject, NULL);
    cog_walk(COG_GLOBALS.on_enter_sym, markobject, NULL);
    COG_GLOBALS.freelist = NULL;
    COG_GLOBALS.freespace = 0;
    for (chunk** c = &COG_GLOBALS.mem; *c;) {
        bool is_empty = true;
        cog_object* freelist_here = COG_GLOBALS.freelist;
        for (size_t i = 0; i < COG_MEM_CHUNK_SIZE; i++) {
            cog_object* o = &(*c)->mem[i];
            if (!o->marked) {
                if (o->type && o->type->destroy) o->type->destroy(o);
                memset(o, 0, sizeof(*o));
                o->next = COG_GLOBALS.freelist;
                COG_GLOBALS.freelist = o;
                COG_GLOBALS.freespace++;
            }
            else {
                o->marked = false;
                is_empty = false;
            }
        }
        if (is_empty) {
            chunk* going = *c;
            *c = (*c)->next;
            cog_free(going);
            COG_GLOBALS.freelist = freelist_here;
            COG_GLOBALS.freespace -= COG_MEM_CHUNK_SIZE;
            COG_GLOBALS.alloc_chunks--;
        } else {
            c = &(*c)->next;
        }
    }
}

size_t cog_get_num_cells_used() {
    return COG_GLOBALS.alloc_chunks * COG_MEM_CHUNK_SIZE - COG_GLOBALS.freespace;
}

double cog_get_gc_fragmentation() {
    size_t used = cog_get_num_cells_used();
    size_t allocated = used + COG_GLOBALS.freespace;
    return (double)allocated / (double)used;
}

void cog_quit() {
    COG_GLOBALS.gc_protected = NULL;
    COG_GLOBALS.stdout_stream = NULL;
    COG_GLOBALS.stdin_stream = NULL;
    COG_GLOBALS.stderr_stream = NULL;
    COG_GLOBALS.modules = NULL;
    COG_GLOBALS.command_queue = NULL;
    COG_GLOBALS.stack = NULL;
    COG_GLOBALS.scopes = NULL;
    COG_GLOBALS.error_sym = NULL;
    COG_GLOBALS.not_impl_sym = NULL;
    COG_GLOBALS.on_enter_sym = NULL;
    COG_GLOBALS.on_exit_sym = NULL;
    gc();
    assert(COG_GLOBALS.mem == NULL);
}

// MARK: MODULES

void free_pointer(cog_object* going) {
    free((FILE*)going->as_ptr);
}

cog_obj_type cog_ot_pointer = {"Pointer", NULL};
cog_obj_type cog_ot_owned_pointer = {"OwnedPointer", NULL, free_pointer};

void cog_add_module(cog_module* module) {
    cog_object* modobj = cog_make_obj(&cog_ot_pointer);
    modobj->as_ptr = (void*)module;
    cog_push_to(&COG_GLOBALS.modules, modobj);
}

// MARK: UTILITY

cog_object* cog_expect_type_fatal(cog_object* obj, cog_obj_type* t) {
    assert(obj->type == t);
    return obj;
}

cog_object* cog_assoc(cog_object* list, cog_object* key, cog_chk_eq_fun same) {
    COG_ITER_LIST(list, pair) {
        if (pair == NULL) continue;
        if (same(key, pair->data)) return pair;
    }
    return NULL;
}

bool cog_same_pointer(cog_object* a, cog_object* b) {
    return a == b;
}

// MARK: LISTS

cog_obj_type cog_ot_list = {"List", cog_walk_both, NULL};

void cog_push_to(cog_object** stack, cog_object* item) {
    cog_object* cell = cog_make_obj(&cog_ot_list);
    cell->data = item;
    cell->next = *stack;
    *stack = cell;
}

cog_object* cog_pop_from(cog_object** stack) {
    if (*stack == NULL) return NULL;
    cog_object* top = *stack;
    *stack = top->next;
    return top->data;
}

cog_object* cog_clone_list_shallow(cog_object* list) {
    if (list == NULL) return NULL;
    cog_obj_type* t = list->type;
    cog_object* out = cog_make_obj(t);
    cog_object* tail = out;
    while (list) {
        tail->data = list->data;
        tail->next = cog_make_obj(t);
        tail = tail->next;
        list = list->next;
    }
    tail->next = NULL;
    return out;
}

cog_object* cog_list_splice(cog_object** l1, cog_object* l2) {
    cog_object* tail = *l1;
    if (tail) {
        assert(!l2 || tail->type == l2->type);
        while (tail->next) tail = tail->next;
        tail->next = l2;
    }
    else {
        *l1 = l2;
    }
    return *l1;
}

void cog_reverse_list_inplace(cog_object** list) {
    cog_object* prev = NULL;
    cog_object* curr = *list;
    cog_object* next;
    while (curr) {
        next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }
    *list = prev;
}

cog_object* cog_hash(cog_object* obj) {
    cog_object* ret = cog_run_well_known(obj, COG_M_HASH);
    if (cog_same_identifiers(cog_not_implemented(), ret)) return ret;
    return cog_pop();
}

// MARK: ENVIRONMENT

void cog_defun(cog_object* identifier, cog_object* value) {
    cog_object* top_scope = COG_GLOBALS.scopes->data;
    cog_object* pair = cog_assoc(top_scope, identifier, cog_same_identifiers);
    if (pair) {
        pair->next = value;
    } else {
        pair = cog_make_obj(&cog_ot_list);
        pair->data = identifier;
        pair->next = value;
        cog_push_to(&COG_GLOBALS.scopes->data, pair);
    }
}

cog_object* cog_get_fun(cog_object* identifier, bool* found) {
    COG_ITER_LIST(COG_GLOBALS.scopes, scope) {
        cog_object* pair = cog_assoc(scope, identifier, cog_same_identifiers);
        if (pair) {
            *found = true;
            return pair->next;
        }
    }
    *found = false;
    return NULL;
}

void cog_push_new_scope() {
    cog_push_scope(cog_make_obj(&cog_ot_list));
}

void cog_push_scope(cog_object* scope) {
    cog_push_to(&COG_GLOBALS.scopes, scope);
}

void cog_pop_scope() {
    cog_pop_from(&COG_GLOBALS.scopes);
}

// MARK: CORE OF VM

void cog_push(cog_object* item) {
    cog_push_to(&COG_GLOBALS.stack, item);
}

cog_object* cog_pop() {
    return cog_pop_from(&COG_GLOBALS.stack);
}

size_t cog_stack_length() {
    size_t len = 0;
    COG_ITER_LIST(COG_GLOBALS.stack, _) len++;
    return len;
}

bool cog_stack_has_at_least(size_t n) {
    size_t len = 0;
    COG_ITER_LIST(COG_GLOBALS.stack, _) {
        len++;
        if (len >= n) return true;
    }
    return false;
}

bool cog_is_stack_empty() {
    return COG_GLOBALS.stack == NULL;
}

void cog_run_next(cog_object* item, cog_object* when, cog_object* cookie) {
    cog_push_to(&cookie, item);
    cog_push_to(&cookie, when);
    cog_push_to(&COG_GLOBALS.command_queue, cookie);
}

cog_object* cog_run_well_known(cog_object* obj, enum cog_well_known_method meth) {
    assert(obj != NULL);
    COG_ITER_LIST(COG_GLOBALS.modules, modobj) {
        cog_module* mod = (cog_module*)modobj->as_ptr;
        if (mod->mtab == NULL) continue;
        for (size_t i = 0; mod->mtab[i] != NULL; i++) {
            cog_object_method* m = mod->mtab[i];
            if (m->wkm == meth && m->type_for == obj->type) {
                cog_push(obj);
                cog_object* res = m->func();
                if (res && cog_same_identifiers(res, COG_GLOBALS.not_impl_sym)) continue;
                return res;
            }
        }
    }
    return cog_not_implemented();
}

cog_object* cog_run_well_known_strict(cog_object* obj, enum cog_well_known_method meth) {
    cog_object* res = cog_run_well_known(obj, meth);
    if (res && cog_same_identifiers(res, COG_GLOBALS.not_impl_sym)) {
        const char* method_name;
        switch (meth) {
            case COG_M_EXEC: method_name = "COG_M_RUN_SELF"; break;
            case COG_M_SHOW: method_name = "COG_M_STRINGIFY_SELF"; break;
            case COG_SM_PUTS: method_name = "COG_SM_PUTS"; break;
            case COG_SM_GETCH: method_name = "COG_SM_GETCH"; break;
            case COG_SM_UNGETS: method_name = "COG_SM_UNGETS"; break;
            default: method_name = "UNKNOWN_METHOD"; break;
        }
        fprintf(stderr, "error: %s not implemented for %s\n", method_name, obj->type ? obj->type->name : "NULL");
        COG_ITER_LIST(COG_GLOBALS.modules, modobj) {
            cog_module* mod = (cog_module*)modobj->as_ptr;
            if (mod->types == NULL) continue;
            for (size_t i = 0; mod->types[i] != NULL; i++) {
                cog_obj_type* t = mod->types[i];
                if (t == obj->type) goto hasimport;
            }
        }
        fprintf(stderr, "did you forget to import the module?\n");
        hasimport:
        print_backtrace();
        abort();
    }
    return res;
}

cog_object* cog_mainloop(cog_object* status) {
    size_t next_gc = COG_GLOBALS.alloc_chunks * 2;
    while (COG_GLOBALS.command_queue) {
        cog_object* cmd = cog_pop_from(&COG_GLOBALS.command_queue);
        if (cmd == NULL) {
            fprintf(stderr, "got NULL as object in command queue\n");
            abort();
        }
        cog_object* when = cmd->data;
        cog_object* which = cmd->next->data;
        cog_object* cookie = cmd->next->next;
        if (which == NULL) {
            fprintf(stderr, "got NULL as command in command queue\n");
            abort();
        }
        bool is_normal_exec = cog_same_identifiers(status, when);
        if (is_normal_exec
                || cog_same_identifiers(cog_on_exit(), when)
                || cog_same_identifiers(cog_on_enter(), when)) {
            cog_push(cookie);
            cog_object* new_status = cog_run_well_known(which, COG_M_EXEC);
            if (cog_same_identifiers(new_status, cog_not_implemented())) {
                cog_pop();
                cog_push(cog_sprintf("Can't run %O", which));
                new_status = cog_error();
            }
            if (is_normal_exec) status = new_status;
            // maybe do a GC
            if (COG_GLOBALS.alloc_chunks > next_gc) {
                // protect status in case it is nonstandard
                cog_walk(status, markobject, NULL);
                gc();
                next_gc = COG_GLOBALS.alloc_chunks * 2;
            }
        }
    }
    return status;
}

cog_object* cog_obj_push_self() {
    cog_object* self = cog_pop();
    cog_pop(); // ignore cookie
    cog_push(self);
    return NULL;
}

// MARK: NUMBERS

cog_obj_type cog_ot_int = {"Integer", NULL};
cog_object* cog_box_int(int64_t i) {
    cog_object* obj = cog_make_obj(&cog_ot_int);
    obj->as_int = i;
    return obj;
}
int64_t cog_unbox_int(cog_object* obj) {
    assert(obj->type == &cog_ot_int);
    return obj->as_int;
}
cog_object* int_printself() {
    cog_object* num = cog_pop();
    cog_pop(); // ignore cookie
    char buffer[21];
    snprintf(buffer, sizeof(buffer), "%" PRId64, num->as_int);
    cog_push(cog_string(buffer));
    return NULL;
}
cog_object_method ome_int_show = {&cog_ot_int, COG_M_SHOW, int_printself};
cog_object_method ome_int_exec = {&cog_ot_int, COG_M_EXEC, cog_obj_push_self};

static cog_object* m_int_hash() {
    // Integers hash to themselves
    return NULL;
}
cog_object_method ome_int_hash = {&cog_ot_int, COG_M_HASH, m_int_hash};

cog_obj_type cog_ot_bool = {"Boolean", NULL};
cog_object* cog_box_bool(bool i) {
    cog_object* obj = cog_make_obj(&cog_ot_bool);
    obj->as_int = i;
    return obj;
}
bool cog_unbox_bool(cog_object* obj) {
    assert(obj->type == &cog_ot_bool);
    return obj->as_int;
}
cog_object* bool_printself() {
    cog_object* num = cog_pop();
    cog_pop(); // ignore cookie
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%s", num->as_int ? "True" : "False");
    cog_push(cog_string(buffer));
    return NULL;
}
cog_object_method ome_bool_show = {&cog_ot_bool, COG_M_SHOW, bool_printself};
cog_object_method ome_bool_exec = {&cog_ot_bool, COG_M_EXEC, cog_obj_push_self};

static cog_object* m_bool_hash() {
    cog_object* num = cog_pop();
    int64_t hash = num->as_int;
    // Booleans hash like integers
    cog_push(cog_box_int(hash));
    return NULL;
}
cog_object_method ome_bool_hash = {&cog_ot_bool, COG_M_HASH, m_bool_hash};

cog_obj_type cog_ot_float = {"Number", NULL};
cog_object* cog_box_float(double i) {
    cog_object* obj = cog_make_obj(&cog_ot_float);
    obj->as_float = i;
    return obj;
}
double cog_unbox_float(cog_object* obj) {
    assert(obj->type == &cog_ot_float);
    return obj->as_float;
}
cog_object* float_printself() {
    cog_object* num = cog_pop();
    cog_pop(); // ignore cookie
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lg", num->as_float);
    cog_push(cog_string(buffer));
    return NULL;
}
cog_object_method ome_float_show = {&cog_ot_float, COG_M_SHOW, float_printself};
cog_object_method ome_float_exec = {&cog_ot_float, COG_M_EXEC, cog_obj_push_self};

static cog_object* m_float_hash() {
    cog_object* num = cog_pop();
    double val = num->as_float;
    // floats hash to their int value if it's an int, otherwise the reinterpret_cast of their bits
    int64_t hash = val == ((double)((int64_t)val)) ? (int64_t)val : *(int64_t*)&val;
    cog_push(cog_box_int(hash));
    return NULL;
}
cog_object_method ome_float_hash = {&cog_ot_float, COG_M_HASH, m_float_hash};

// MARK: IDENTIFIERS

static cog_object* walk_identifier(cog_object* i, cog_walk_fun f, cog_object* arg) {
    (void)arg;
    if (i->as_int) return NULL;
    return i->next;
}
cog_obj_type cog_ot_identifier = {"Identifier", walk_identifier};

// TODO: parameterize / calculate this constant at compile time
#define MAXPACKED 11
const char* const PACKEDALPHABET = "0123456789abcdefghijklmnopqrstuvwxyz-?!'+/\\*>=<^.";

static bool pack_identifier_c(const char* const buffer, cog_packed_identifier* out) {
    cog_packed_identifier res = 0;
    size_t len = 0;
    size_t base = strlen(PACKEDALPHABET);
    for (const char* s = buffer; *s; s++, len++) {
        if (len > MAXPACKED) return false;
        // !!! identifiers are case insensitive
        const char* where = strchr(PACKEDALPHABET, tolower(*s));
        if (where == NULL) return false;
        res *= base;
        res += where - PACKEDALPHABET;
    }
    *out = (res << 1) | 1;
    return true;
}

static bool pack_identifier(cog_object* string, cog_packed_identifier* out) {
    cog_packed_identifier res = 0;
    size_t len = cog_strlen(string);
    if (len > MAXPACKED) return false;
    size_t base = strlen(PACKEDALPHABET);
    for (size_t i = 0; i < len; i++) {
        // !!! identifiers are case insensitive
        const char* where = strchr(PACKEDALPHABET, tolower(cog_nthchar(string, i)));
        if (where == NULL) return false;
        res *= base;
        res += where - PACKEDALPHABET;
    }
    *out = (res << 1) | 1;
    return true;
}

cog_object* cog_explode_identifier(cog_object* i, bool cap_first) {
    cog_object* buffer = cog_emptystring();
    cog_object* tail = buffer;
    if (i->as_packed_sym & 1) {
        // packed identifier
        cog_packed_identifier s = i->as_packed_sym >> 1;
        size_t base = strlen(PACKEDALPHABET);
        cog_packed_identifier div = 1;
        while ((div * base) < s)
            div *= base;
        int (*tr)(int) = cap_first ? toupper : tolower;
        for (;;) {
            cog_string_append_byte(&tail, tr(PACKEDALPHABET[(s / div) % base]));
            if (div == 1) break;
            div /= base;
            tr = tolower;
        }
    } else if (i->as_fun != NULL) {
        // builtin identifier
        for (const char* s = i->as_fun->name; *s; s++)
            cog_string_append_byte(&tail, *s);
    } else {
        // long identifier
        buffer = i->next;
    }
    return buffer;
}

cog_object* cog_make_identifier_c(const char* const name) {
    // TODO: intern?
    cog_object* out = cog_make_obj(&cog_ot_identifier);
    // first try the builtin function names
    COG_ITER_LIST(COG_GLOBALS.modules, modobj) {
        cog_module* mod = (cog_module*)modobj->as_ptr;
        if (mod->table == NULL) continue;
        for (size_t i = 0; mod->table[i] != NULL; i++) {
            cog_modfunc* m = mod->table[i];
            // !!! not case sensitive
            if ((m->when == COG_FUNC || m->when == COG_COOKIEFUNC) && !strcasecmp(m->name, name)) {
                // found desired modfunc
                out->as_fun = m;
                goto done;
            }
        }
    }
    // then try packed identifier
    cog_packed_identifier packed;
    if (pack_identifier_c(name, &packed)) {
        out->as_packed_sym = packed;
        goto done;
    }
    // then default to long identifier
    out->next = cog_string(name);
    done:
    return out;
}

cog_object* cog_make_identifier(cog_object* string) {
    cog_object* out = cog_make_obj(&cog_ot_identifier);
    // first try the builtin function names
    COG_ITER_LIST(COG_GLOBALS.modules, modobj) {
        cog_module* mod = (cog_module*)modobj->as_ptr;
        if (mod->table == NULL) continue;
        for (size_t i = 0; mod->table[i] != NULL; i++) {
            cog_modfunc* m = mod->table[i];
            // !!! not case sensitive
            if ((m->when == COG_FUNC || m->when == COG_COOKIEFUNC) && !cog_strcasecmp_c(string, m->name)) {
                // found desired modfunc
                out->as_fun = m;
                goto done;
            }
        }
    }
    // then try packed identifier
    cog_packed_identifier packed;
    if (pack_identifier(string, &packed)) {
        out->as_packed_sym = packed;
        goto done;
    }
    // then default to long identifier
    out->next = string;
    done:
    return out;
}

cog_object* m_show_identifier() {
    cog_object* i = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_explode_identifier(i, true));
    return NULL;
}
cog_object_method ome_identifier_show = {
    &cog_ot_identifier,
    COG_M_SHOW,
    m_show_identifier
};

cog_object* m_run_identifier() {
    cog_object* self = cog_pop();
    cog_object* cookie = cog_pop();
    // first look up definition
    bool found = false;
    cog_object* def = cog_get_fun(self, &found);
    if (found) {
        // push the definition instead
        cog_run_next(def, NULL, cookie);
        return NULL;
    } else {
        // use builtin identifier if available or throw undefined
        if (!self->next && (self->as_packed_sym & 1) == 0 && self->as_packed_sym != 0) {
            cog_run_next(cog_make_bfunction(self->as_fun), NULL, cookie);
            return NULL;
        } else {
            cog_push(cog_sprintf("undefined: %O", self));
            return cog_error();
        }
    }
}
cog_object_method ome_identifier_exec = {&cog_ot_identifier, COG_M_EXEC, m_run_identifier};

static int64_t _string_hash(cog_object*, int64_t);

static cog_object* m_identifier_hash() {
    cog_push(cog_box_int(_string_hash(cog_explode_identifier(cog_pop(), true), 14695981039346656039ULL)));
    return NULL;
}
cog_object_method ome_identifier_hash = {&cog_ot_identifier, COG_M_HASH, m_identifier_hash};

bool cog_same_identifiers(cog_object* s1, cog_object* s2) {
    if (!s1 && !s2) return true;
    if (!s1 || !s2) return false;
    assert(s1->type == &cog_ot_identifier);
    assert(s2->type == &cog_ot_identifier);
    return !cog_strcasecmp(cog_explode_identifier(s1, false), cog_explode_identifier(s2, false));
}

// MARK: SYMBOLS

cog_obj_type cog_ot_symbol = {"Symbol", cog_walk_only_next, NULL};

cog_object_method ome_symbol_exec = {&cog_ot_symbol, COG_M_EXEC, cog_obj_push_self};

cog_object* cog_sym(cog_object* i) {
    cog_object* s = cog_make_obj(&cog_ot_symbol);
    s->next = i;
    return s;
}

cog_object* m_symbol_show() {
    cog_object* sym = cog_pop();
    bool readably = cog_expect_type_fatal(cog_pop(), &cog_ot_bool)->as_int;
    cog_object* chars = cog_explode_identifier(sym->next, false);
    if (!readably) {
        cog_push(chars);
    } else {
        cog_push(cog_sprintf("\\%#O", chars));
    }
    return NULL;
}
cog_object_method ome_symbol_show = {&cog_ot_symbol, COG_M_SHOW, m_symbol_show};

static cog_object* m_symbol_hash() {
    cog_push(cog_box_int(_string_hash(cog_explode_identifier(cog_pop()->next, false), 14695981039346656035ULL)));
    return NULL;
}
cog_object_method ome_symbol_hash = {&cog_ot_symbol, COG_M_HASH, m_symbol_hash};

// MARK: STRINGS

cog_obj_type cog_ot_string = {"String", cog_walk_only_next, NULL};

cog_object* cog_emptystring() {
    return cog_make_obj(&cog_ot_string);
}

void cog_string_append_byte(cog_object** str, char data) {
    if ((*str)->stored_chars >= COG_MAX_CHARS_PER_BUFFER_CHUNK) {
        while ((*str)->next != NULL) {
            *str = (*str)->next;
        }
        cog_object* next = cog_emptystring();
        next->as_chars[0] = data;
        next->stored_chars = 1;
        (*str)->next = next;
        *str = next;
    }
    else {
        (*str)->as_chars[(*str)->stored_chars] = data;
        (*str)->stored_chars++;
    }
}

void cog_string_prepend_byte(cog_object** str, char data) {
    if ((*str) && (*str)->stored_chars < COG_MAX_CHARS_PER_BUFFER_CHUNK) {
        memmove((*str)->as_chars + 1, (*str)->as_chars, (*str)->stored_chars);
        (*str)->as_chars[0] = data;
        (*str)->stored_chars++;
    }
    else {
        cog_object* new_head = cog_emptystring();
        new_head->as_chars[0] = data;
        new_head->stored_chars = 1;
        new_head->next = *str;
        *str = new_head;
    }
}

size_t cog_strlen(cog_object* str) {
    size_t len = 0;
    while (str != NULL) {
        len += str->stored_chars;
        str = str->next;
    }
    return len;
}

char cog_nthchar(cog_object* str, size_t i) {
    while (i >= str->stored_chars) {
        if (str == NULL) return 0;
        i -= str->stored_chars;
        str = str->next;
    }
    if (!str || i >= str->stored_chars) return 0;
    return str->as_chars[i];
}

void _str_set_nthchar(cog_object* str, size_t i, char c) {
    while (i >= str->stored_chars) {
        assert(str != NULL);
        i -= str->stored_chars;
        str = str->next;
    }
    assert(str && i < str->stored_chars);
    str->as_chars[i] = c;
}

void cog_string_insert_char(cog_object** str, char data, size_t index) {
    cog_object* current = *str;

    while (current && index >= current->stored_chars) {
        index -= current->stored_chars;
        current = current->next;
    }

    if (!current) {
        // If index is out of bounds, append to the end
        cog_string_append_byte(str, data);
        return;
    }

    if (current->stored_chars < COG_MAX_CHARS_PER_BUFFER_CHUNK) {
        memmove(current->as_chars + index + 2, current->as_chars + index + 1, current->stored_chars - index);
        current->as_chars[index] = data;
        current->stored_chars++;
    } else {
        cog_string_prepend_byte(&current->next, current->as_chars[COG_MAX_CHARS_PER_BUFFER_CHUNK - 1]);
        memmove(current->as_chars + index + 2, current->as_chars + index + 1, current->stored_chars - index);
        current->stored_chars = COG_MAX_CHARS_PER_BUFFER_CHUNK;
        current->as_chars[index] = data;
    }
}

void cog_string_delete_char(cog_object** str, size_t index) {
    cog_object** current = str;

    while (*current && index >= (*current)->stored_chars) {
        index -= (*current)->stored_chars;
        current = &(*current)->next;
    }

    if (!*current) {
        // If index is out of bounds, do nothing
        return;
    }

    memmove((*current)->as_chars + index, (*current)->as_chars + index + 1, (*current)->stored_chars - index - 1);
    (*current)->stored_chars--;

    if ((*current)->stored_chars == 0) {
        *current = (*current)->next;
    }
}

cog_object* cog_substring(cog_object* str, size_t start, size_t end) {
    while (str && start >= str->stored_chars) {
        start -= str->stored_chars;
        end -= str->stored_chars;
        str = str->next;
    }
    if (!str) return NULL;
    cog_object* out = cog_strdup(str);
    while (start > 0) {
        cog_string_delete_char(&out, 0);
        start--;
        end--;
    }
    cog_object* end_chunk = out;
    while (end_chunk && end >= end_chunk->stored_chars) {
        end -= end_chunk->stored_chars;
        end_chunk = end_chunk->next;
    }
    if (end_chunk) {
        end_chunk->stored_chars = end;
        end_chunk->next = NULL;
    }
    return out;
}

cog_object* cog_string(const char* const cstr) {
    return cog_string_from_bytes(cstr, strlen(cstr));
}

cog_object* cog_string_from_bytes(const char* const cstr, size_t n) {
    cog_object* str = cog_emptystring();
    cog_object* tail = str;
    for (size_t i = 0; i < n; i++)
        cog_string_append_byte(&tail, cstr[i]);
    return str;
}

size_t cog_string_to_cstring(cog_object* str, char* const cstr, size_t len) {
    char* p = cstr;
    while (str && (p - cstr) < len) {
        size_t nmore = min(str->stored_chars, len - (p - cstr));
        memcpy(p, str->as_chars, nmore);
        p += nmore;
        str = str->next;
    }
    *p = 0;
    return p - cstr;
}

cog_object* m_string_show() {
    cog_object* buffer = cog_pop();
    bool readably = cog_expect_type_fatal(cog_pop(), &cog_ot_bool)->as_int;
    if (!readably) {
        cog_push(buffer);
    }
    else {
        cog_object* ebuf = cog_emptystring();
        cog_object* tail = ebuf;
        cog_string_append_byte(&tail, '"');
        cog_object* chunk = buffer;
        while (chunk) {
            for (size_t i = 0; i < chunk->stored_chars; i++) {
                bool special = false;
                char ch = cog_maybe_escape_char(chunk->as_chars[i], &special);
                if (special) cog_string_append_byte(&tail, '\\');
                cog_string_append_byte(&tail, ch);
            }
            chunk = chunk->next;
        }
        cog_string_append_byte(&tail, '"');
        cog_push(ebuf);
    }
    return NULL;
}
cog_object_method ome_string_show = {&cog_ot_string, COG_M_SHOW, m_string_show};
cog_object_method ome_string_exec = {&cog_ot_string, COG_M_EXEC, cog_obj_push_self};

static int64_t _string_hash(cog_object* str, int64_t hash) {
    while (str) {
        for (int i = 0; i < str->stored_chars; i++) {
            hash ^= (int64_t)str->as_chars[i];
            hash *= 1099511628211ULL;
        }
        str = str->next;
    }
    return hash;
}

static cog_object* m_string_hash() {
    cog_push(cog_box_int(_string_hash(cog_pop(), 14695981039346656037ULL)));
    return NULL;
}
cog_object_method ome_string_hash = {&cog_ot_string, COG_M_HASH, m_string_hash};

cog_object* cog_make_character(char c) {
    // a character is just a one character string
    cog_object* obj = cog_emptystring();
    cog_string_append_byte(&obj, c);
    return obj;
}

cog_object* cog_strappend(cog_object* str1, cog_object* str2) {
    cog_object* str1clone = cog_strdup(str1);
    cog_object* str12 = cog_strcat(&str1clone, str2);
    assert(cog_strlen(str12) == cog_strlen(str1) + cog_strlen(str2));
    return str12;
}

int cog_strcmp(cog_object* str1, cog_object* str2) {
    return cog_strncmp(str1, str2, SIZE_MAX);
}

int cog_strncmp(cog_object* str1, cog_object* str2, size_t n) {
    assert(!str1 || str1->type == &cog_ot_string);
    assert(!str2 || str2->type == &cog_ot_string);
    int i1 = 0, i2 = 0;
    while (str1 && str2 && n > 0) {
        char c1 = str1->as_chars[i1];
        char c2 = str2->as_chars[i2];
        char d = c1 - c2;
        if (d != 0) return d;
        i1++, i2++, n--;
        while (str1 && i1 >= str1->stored_chars) {
            i1 -= str1->stored_chars;
            str1 = str1->next;
        }
        while (str2 && i2 >= str2->stored_chars) {
            i2 -= str2->stored_chars;
            str2 = str2->next;
        }
    }
    if (n == 0) return 0;
    if (!str1 && !str2) return 0;
    if (!str1) return -1;
    if (!str2) return 1;
    return 0;
}

int cog_strcasecmp(cog_object* str1, cog_object* str2) {
    assert(str1->type == &cog_ot_string);
    assert(str2->type == &cog_ot_string);
    int i1 = 0, i2 = 0;
    while (str1 && str2) {
        char d = tolower(str1->as_chars[i1]) - tolower(str2->as_chars[i2]);
        if (d != 0) return d;
        i1++, i2++;
        while (str1 && i1 >= str1->stored_chars) {
            i1 = 0;
            str1 = str1->next;
        }
        while (str2 && i2 >= str2->stored_chars) {
            i2 = 0;
            str2 = str2->next;
        }
    }
    if (!str1 && !str2) return 0;
    if (str1) return 1;
    return -1;
}

int cog_strcmp_c(cog_object* str1, const char* const str2) {
    assert(str1->type == &cog_ot_string);
    int i1 = 0;
    const char* p = str2;
    while (str1 && *p) {
        char d = str1->as_chars[i1] - *p;
        if (d != 0) return d;
        i1++, p++;
        if (i1 >= str1->stored_chars) {
            i1 = 0;
            str1 = str1->next;
        }
    }
    if (!str1 && !*p) return 0;
    if (str1) return 1;
    return -1;
}

int cog_strcasecmp_c(cog_object* str1, const char* const str2) {
    assert(str1->type == &cog_ot_string);
    int i1 = 0;
    const char* p = str2;
    while (str1 && *p) {
        char d = tolower(str1->as_chars[i1]) - tolower(*p);
        if (d != 0) return d;
        i1++, p++;
        if (i1 >= str1->stored_chars) {
            i1 = 0;
            str1 = str1->next;
        }
    }
    if (!str1 && !*p) return 0;
    if (str1) return 1;
    return -1;
}

// MARK: GENERAL STREAM STUFF

cog_obj_type ot_eof = {"EOF", NULL};

cog_object* cog_eof() {
    return cog_make_obj(&ot_eof);
}

char cog_getch(cog_object* file) {
    cog_run_well_known_strict(file, COG_SM_GETCH);
    cog_object* ch = cog_pop();
    if (ch && ch->type == &ot_eof) return EOF;
    return cog_nthchar(ch, 0);
}

void cog_fputs_imm(cog_object* stream, const char* const string) {
    cog_push(cog_string(string));
    cog_run_well_known_strict(stream, COG_SM_PUTS);
}

void cog_fputchar_imm(cog_object* stream, char ch) {
    cog_push(cog_make_character(ch));
    cog_run_well_known_strict(stream, COG_SM_PUTS);
}

void cog_ungetch(cog_object* file, char ch) {
    cog_push(cog_make_character(ch));
    cog_run_well_known_strict(file, COG_SM_UNGETS);
}

// MARK: STRING STREAMS

cog_obj_type ot_iostring = {"IOString", cog_walk_both, NULL};
cog_object* cog_empty_io_string() {
    cog_object* stream = cog_make_obj(&ot_iostring);
    stream->data = cog_box_int(0); // the cursor position
    stream->next = cog_make_obj(&cog_ot_list);
    stream->next->data = cog_emptystring(); // the ungetc stack
    stream->next->next = cog_emptystring(); // the actual contents
    return stream;
}

cog_object* cog_iostring_wrap(cog_object* string) {
    cog_object* stream = cog_empty_io_string();
    stream->next->next = string;
    return stream;
}

cog_object* cog_iostring_get_contents(cog_object* stream) {
    assert(stream && stream->type == &ot_iostring);
    return stream->next->next;
}

static cog_object* m_iostring_write() {
    cog_object* stream = cog_pop();
    cog_object* buf = cog_expect_type_fatal(cog_pop(), &cog_ot_string);
    if (cog_strlen(stream->next->data) > 0) {
        cog_push(cog_string("can't write until ungets stack is empty"));
        return cog_error();
    }
    int64_t pos = stream->data->as_int;
    cog_object* data = cog_iostring_get_contents(stream);
    cog_object* tail = data;
    size_t len = cog_strlen(data);
    while (buf) {
        for (int i = 0; i < buf->stored_chars; i++) {
            if (pos < len) _str_set_nthchar(data, pos, buf->as_chars[i]);
            else cog_string_append_byte(&tail, buf->as_chars[i]);
            pos++;
        }
        buf = buf->next;
    }
    stream->data = cog_box_int(pos);
    return NULL;
}
cog_object_method ome_iostring_write = {&ot_iostring, COG_SM_PUTS, m_iostring_write};

cog_object* m_iostring_getch() {
    cog_object* stream = cog_pop();
    if (cog_strlen(stream->next->data) > 0) {
        char c = cog_nthchar(stream->next->data, 0);
        cog_string_delete_char(&stream->next->data, 0);
        cog_push(cog_make_character(c));
        return NULL;
    }
    int64_t pos = stream->data->as_int;
    cog_object* data = cog_iostring_get_contents(stream);
    size_t len = cog_strlen(data);
    if (pos < len) {
        cog_push(cog_make_character(cog_nthchar(data, pos)));
        stream->data = cog_box_int(pos + 1);
    } else {
        cog_push(cog_eof());
    }
    return NULL;
}
cog_object_method ome_iostring_getch = {&ot_iostring, COG_SM_GETCH, m_iostring_getch};

cog_object* m_iostring_ungets() {
    cog_object* stream = cog_pop();
    cog_object* buf = cog_expect_type_fatal(cog_pop(), &cog_ot_string);
    size_t len = cog_strlen(buf);
    for (size_t iplus1 = len; iplus1 > 0; iplus1--) {
        cog_string_prepend_byte(&stream->next->data, cog_nthchar(buf, iplus1 - 1));
    }
    return NULL;
}
cog_object_method ome_iostring_ungets = {&ot_iostring, COG_SM_UNGETS, m_iostring_ungets};

cog_object* m_iostring_show() {
    cog_object* stream = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<IOstring at pos %O of %O>", stream->data, cog_iostring_get_contents(stream)));
    return NULL;
}
cog_object_method ome_iostring_show = {&ot_iostring, COG_M_SHOW, m_iostring_show};

cog_object_method ome_iostring_hash = {&ot_iostring, COG_M_HASH, cog_not_implemented};

// MARK: BUILTIN FUNCTION OBJECTS

cog_obj_type ot_bfunction = {"BuiltinFunction", NULL};

cog_object* cog_make_bfunction(cog_modfunc* func) {
    assert(func);
    assert(func->when == COG_FUNC || func->when == COG_COOKIEFUNC);
    cog_object* obj = cog_make_obj(&ot_bfunction);
    obj->as_fun = func;
    return obj;
}

cog_object* m_bfunction_exec() {
    cog_object* self = cog_pop();
    cog_modfunc* f = self->as_fun;
    if (f->when == COG_FUNC) cog_pop(); // discard cookie
    // jump straight into the function
    return f->fun();
}
cog_object_method ome_bfunction_exec = {&ot_bfunction, COG_M_EXEC, m_bfunction_exec};

cog_object_method ome_bfunction_hash = {&ot_bfunction, COG_M_HASH, cog_not_implemented};

// MARK: BLOCKS AND CLOSURES

cog_obj_type ot_block = {"Block", cog_walk_only_next, NULL};
cog_obj_type ot_closure = {"Closure", cog_walk_both, NULL};

cog_object* cog_make_block(cog_object* commands) {
    assert(!commands || commands->type == &cog_ot_list);
    cog_object* obj = cog_make_obj(&ot_block);
    obj->next = commands;
    return obj;
}

cog_object* cog_make_closure(cog_object* block, cog_object* scopes) {
    assert(block && block->type == &ot_block);
    assert(!scopes || scopes->type == &cog_ot_list);
    cog_object* obj = cog_make_obj(&ot_closure);
    obj->data = block;
    obj->next = scopes;
    return obj;
}

cog_object* m_closure_exec() {
    cog_object* self = cog_pop();
    cog_object* cookie = cog_pop();
    if (!cookie) cookie = cog_box_bool(true);
    bool should_push_scope = cog_expect_type_fatal(cookie, &cog_ot_bool)->as_int;
    // push scope teardown command
    if (should_push_scope) cog_run_next(cog_make_identifier_c("[[Closure::RestoreCallerScope]]"), cog_on_exit(), COG_GLOBALS.scopes);
    // push all current commands
    cog_object* head_existing = COG_GLOBALS.command_queue;
    COG_GLOBALS.command_queue = NULL;
    COG_ITER_LIST(self->data->next, cmd) cog_run_next(cmd, NULL, NULL);
    cog_reverse_list_inplace(&COG_GLOBALS.command_queue);
    cog_list_splice(&COG_GLOBALS.command_queue, head_existing);
    cog_push_to(&cookie, self->next);
    cog_run_next(cog_make_identifier_c("[[Closure::InsertCallScope]]"), cog_on_enter(), cookie);
    return NULL;
}
cog_object_method ome_closure_exec = {&ot_closure, COG_M_EXEC, m_closure_exec};
cog_object_method ome_closure_hash = {&ot_closure, COG_M_HASH, cog_not_implemented};

cog_object* fn_closure_restore_scope() {
    cog_object* old_scope = cog_pop();
    COG_GLOBALS.scopes = old_scope;
    return NULL;
}
cog_modfunc fne_closure_restore_scope = {
    "[[Closure::RestoreCallerScope]]",
    COG_COOKIEFUNC,
    fn_closure_restore_scope,
    NULL,
};

cog_object* fn_closure_insert_new_scope() {
    cog_object* cookie2 = cog_pop();
    cog_object* closed_scopes = cookie2->data;
    bool should_push_new = cookie2->next ? cookie2->next->as_int : false;
    COG_GLOBALS.scopes = closed_scopes;
    if (should_push_new) cog_push_new_scope();
    return NULL;
}
cog_modfunc fne_closure_insert_new_scope = {
    "[[Closure::InsertCallScope]]",
    COG_COOKIEFUNC,
    fn_closure_insert_new_scope,
    NULL,
};

cog_object* m_block_exec() {
    cog_object* self = cog_pop();
    cog_pop(); // ignore cookie
    cog_push(cog_make_closure(self, COG_GLOBALS.scopes));
    return NULL;
}
cog_object_method ome_block_exec = {&ot_block, COG_M_EXEC, m_block_exec};

cog_object* m_block_show() {
    cog_object* block = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<Block %O>", block->next));
    return NULL;
}
cog_object_method ome_block_show = {&ot_block, COG_M_SHOW, m_block_show};
cog_object_method ome_block_hash = {&ot_block, COG_M_HASH, cog_not_implemented};



// MARK: DUMPER

static bool make_refs_list(cog_object* obj, cog_object* alist_header) {
    cog_object* entry = cog_assoc(alist_header->data, obj, cog_same_pointer);
    if (entry) {
        entry->next = cog_box_int(2);
        return false;
    }
    cog_object* pair = cog_make_obj(&cog_ot_list);
    pair->data = obj;
    pair->next = cog_box_int(1);
    cog_push_to(&alist_header->data, pair);
    // plain cons cells are the only "interesting" thing
    return obj->type == NULL;
}

static int64_t reffed(cog_object* obj, cog_object* alist, int64_t* counter) {
    cog_object* entry = cog_assoc(alist, obj, cog_same_pointer);
    if (entry) {
        int64_t value = entry->next->as_int;
        if (value < 0) {
            return value;
        }
        if (value > 1) {
            int64_t my_id = (*counter)++;
            entry->next = cog_box_int(-my_id);
            return my_id;
        }
    }
    return 0;
}

static void pr_refs_recursive(cog_object* obj, cog_object* alist, cog_object* stream, int64_t* counter, bool readably) {
    char buffer[256];
    if (obj == NULL) {
        cog_fputs_imm(stream, "()");
        return;
    }
    // test if it's in the table
    int64_t ref = reffed(obj, alist, counter);
    if (ref < 0) {
        snprintf(buffer, sizeof(buffer), "#%" PRId64 "#", -ref);
        cog_fputs_imm(stream, buffer);
        return;
    }
    if (ref) {
        snprintf(buffer, sizeof(buffer), "#%" PRId64 "=", ref);
        cog_fputs_imm(stream, buffer);
    }
    // TODO: don't special case List
    if (obj->type && obj->type != &cog_ot_list) {
        // TODO: use COG_M_SHOW_REC if available
        cog_push(cog_box_bool(readably));
        if (cog_same_identifiers(cog_run_well_known(obj, COG_M_SHOW), cog_not_implemented())) {
            cog_pop();
            snprintf(buffer, sizeof(buffer), "#<%s: %p %p>", obj->type->name, obj->data, obj->next);
            cog_fputs_imm(stream, buffer);
        } else {
            cog_run_well_known_strict(stream, COG_SM_PUTS);
        }
    }
    else {
        cog_fputchar_imm(stream, '(');
        for (;;) {
            pr_refs_recursive(obj->data, alist, stream, counter, readably);
            obj = obj->next;
            int64_t ref = reffed(obj, alist, counter);
            if (ref) {
                if (ref > 0) {
                    // reset the ref so it will print properly after the .
                    cog_assoc(alist, obj, cog_same_pointer)->data = cog_box_int(ref);
                    (*counter)--;
                }
                break;
            }
            if (obj && obj->type == NULL) cog_fputchar_imm(stream, ' ');
            else break;
        }
        if (obj) {
            cog_fputs_imm(stream, " . ");
            pr_refs_recursive(obj, alist, stream, counter, readably);
        }
        cog_fputchar_imm(stream, ')');
    }
}

void cog_dump(cog_object* obj, cog_object* stream, bool readably) {
    cog_object* alist_header = cog_make_obj(&cog_ot_list);
    int64_t counter = 1;
    cog_walk(obj, make_refs_list, alist_header);
    pr_refs_recursive(obj, alist_header->data, stream, &counter, readably);
}

// MARK: PARSER

const char COG_ESCAPE_MAP[23] = "a\ab\bf\fn\nr\rt\tv\ve\x1b\\\\\"\"z";

char cog_maybe_escape_char(char c, bool* did_escape) {
    const char* p = COG_ESCAPE_MAP;
    while (*p) {
        if (*(p + 1) == c) {
            *did_escape = true;
            return *p;
        }
        p += 2;
    }
    return c;
}

char cog_unescape_char(char c) {
    const char* p = COG_ESCAPE_MAP;
    while (*p) {
        if (*p == c) return *(p + 1);
        p += 2;
    }
    return c;
}

const char doc_parser_internals[] = "Parser Internal -- you should never see this.";

static void run_nextitem_next(cog_object* stream, cog_object* ibuf) {
    cog_object* cookie = cog_emptystring();
    cog_push_to(&cookie, cog_box_int(0));
    cog_push_to(&cookie, ibuf);
    cog_push_to(&cookie, COG_GLOBALS.modules);
    cog_push_to(&cookie, stream);
    cog_run_next(cog_make_identifier_c("[[Parser::NextItem]]"), NULL, cookie);
}

cog_object* fn_parser_handle_token() {
    cog_object* cookie = cog_pop();
    cog_object* stream = cookie->data;
    cog_object* modlist = cookie->next->data;
    cog_object* buffer = cookie->next->next->data;
    cog_object* index = cookie->next->next->next;
    cog_module* curr_mod;
    cog_modfunc* curr_func;
    cog_object* cookie2;
    cog_object* res;
    if (!modlist) goto error;
    // handle what's in the buffer
    curr_mod = (cog_module*)modlist->data->as_ptr;
    if (!curr_mod->table) goto nextmod;
    curr_func = curr_mod->table[index->as_int];
    if (!curr_func) goto nextmod;
    if (curr_func->when != COG_PARSE_TOKEN_HANDLER) goto nextfun;
    if (curr_func->name && buffer->type == &cog_ot_string && cog_strcasecmp_c(buffer, curr_func->name))
        goto nextfun;
    if (curr_func->name && buffer->type != &cog_ot_string) goto nextfun;

    cookie2 = cog_make_obj(&cog_ot_list);
    cookie2->data = buffer;
    cookie2->next = stream;
    cog_push(cookie2);
    res = curr_func->fun();
    if (cog_same_identifiers(res, cog_not_implemented())) {
        cog_pop();
        if (buffer->type == &cog_ot_string && cog_strlen(buffer) == 0) {
            // clearing buffer == no token
            // so try again
            run_nextitem_next(stream, cog_emptystring());
            return NULL;
        }
        goto nextfun;
    }
    return res;

    nextfun:
    cookie->next->next->next = cog_box_int(index->as_int + 1);
    goto retry;

    nextmod:
    cookie->next->next->next = cog_box_int(0);
    cog_pop_from(&cookie->next->data);
    goto retry;

    error:
    cog_push(cog_box_bool(true));
    cog_run_well_known_strict(buffer, COG_M_SHOW);
    COG_RETURN_ERROR(cog_sprintf("PARSE ERROR: could not handle token %O", cog_pop()));

    retry:
    cog_run_next(cog_make_identifier_c("[[Parser::HandleToken]]"), NULL, cookie);
    return NULL;
}
cog_modfunc fne_parser_handle_token = {
    "[[Parser::HandleToken]]",
    COG_COOKIEFUNC,
    fn_parser_handle_token,
    doc_parser_internals,
};

cog_object* fn_parser_nextitem() {
    cog_object* cookie = cog_pop();
    cog_object* stream = cookie->data;
    cog_object* modlist = cookie->next->data;
    cog_object* buffer = cookie->next->next->data;
    cog_object* index = cookie->next->next->next->data;
    cog_object* curr_char = cookie->next->next->next->next;
    cog_object* tail = buffer;
    cog_module* curr_mod;
    cog_modfunc* curr_func;
    cog_object* cookie2;
    cog_object* res;
    cog_object* old_top;
    int ch;

    if (curr_char->type != &ot_eof && cog_strlen(curr_char) != 1) goto firstchar;
    ch = curr_char->type != &ot_eof ? cog_nthchar(curr_char, 0) : EOF;

    if (!modlist) goto nextchar;
    // test current character
    curr_mod = (cog_module*)modlist->data->as_ptr;
    if (!curr_mod->table) goto nextmod;
    curr_func = curr_mod->table[index->as_int];
    if (!curr_func) goto nextmod;
    if (curr_func->when != COG_PARSE_INDIV_CHAR && curr_func->when != COG_PARSE_END_CHAR) goto nextfun;
    if (curr_func->when == COG_PARSE_END_CHAR && cog_strlen(buffer) == 0) goto nextfun;
    if (curr_func->name != NULL && strchr(curr_func->name, ch) == NULL) goto nextfun;

    cookie2 = stream;
    cog_push_to(&cookie2, curr_char);
    cog_push_to(&cookie2, buffer);
    old_top = COG_GLOBALS.stack->data;
    cog_push(cookie2);
    res = curr_func->fun();
    if (cog_same_identifiers(res, cog_not_implemented())) {
        cog_pop();
        goto nextfun;
    }
    assert(COG_GLOBALS.stack->data == old_top);
    goto end_of_token;

    nextfun:
    cookie->next->next->next->data = cog_box_int(index->as_int + 1);
    goto loop;

    nextmod:
    cookie->next->next->next->data = cog_box_int(0);
    cog_pop_from(&cookie->next->data);
    goto loop;

    nextchar:
    if (curr_char->type == &ot_eof && cog_strlen(buffer) == 0) goto end_of_token;
    if (ch != EOF) cog_string_append_byte(&tail, ch);

    firstchar:
    COG_RUN_WKM_RETURN_IF_ERROR(stream, COG_SM_GETCH);
    cookie->next->next->next->next = cog_pop();
    cookie->next->data = COG_GLOBALS.modules;

    loop:
    cog_run_next(cog_make_identifier_c("[[Parser::NextItem]]"), NULL, cookie);
    return NULL;

    end_of_token:
    if (cog_strlen(buffer) == 0 && curr_char->type == &cog_ot_string) buffer = curr_char;
    else if (ch != EOF) cog_ungetch(stream, ch);
    // handle current token
    if (cog_strlen(buffer) == 0) {
        assert(curr_char->type == &ot_eof);
        buffer = curr_char;
    }
    cookie = cog_box_int(0);
    cog_push_to(&cookie, buffer);
    cog_push_to(&cookie, COG_GLOBALS.modules);
    cog_push_to(&cookie, stream);
    cog_run_next(cog_make_identifier_c("[[Parser::HandleToken]]"), NULL, cookie);
    return NULL;
}
cog_modfunc fne_parser_nextitem = {
    "[[Parser::NextItem]]",
    COG_COOKIEFUNC,
    fn_parser_nextitem,
    doc_parser_internals,
};

cog_object* fn_parser_rule_special_chars() {
    cog_object* cookie = cog_pop();
    cog_object* ch = cookie->next->data;
    if (ch->type == &cog_ot_string) {
        char ch = cog_nthchar(cookie->next->data, 0);
        if (isspace(ch)) return NULL;
        if (strchr("([{\"~;", ch)) return NULL;
    }
    if (ch->type == &ot_eof) return NULL;
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_rule_special_chars = {
    NULL,
    COG_PARSE_INDIV_CHAR,
    fn_parser_rule_special_chars,
    doc_parser_internals,
};

cog_object* fn_parser_rule_break_chars() {
    cog_object* cookie = cog_pop();
    cog_object* ch = cookie->next->data;
    if (cookie->next->data->type == &cog_ot_string) {
        char ch = cog_nthchar(cookie->next->data, 0);
        if (strchr("\\)]}", ch)) return NULL;
    }
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_rule_break_chars = {
    NULL,
    COG_PARSE_END_CHAR,
    fn_parser_rule_break_chars,
    doc_parser_internals,
};

cog_object* fn_parser_ignore_whitespace() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        size_t len = cog_strlen(s);
        for (size_t i = 0; i < len; i++) {
            if (!isspace(cog_nthchar(s, i))) {
                cog_push(cookie);
                return cog_not_implemented();
            }
        }
        // clear the token to discard it
        s->stored_chars = 0;
        s->next = NULL;
    }
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_ignore_whitespace = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_ignore_whitespace,
    doc_parser_internals
};

cog_object* fn_parser_handle_int() {
    char buffer[32];
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        cog_string_to_cstring(s, buffer, sizeof(buffer));
        int64_t i;
        int len = 0;
        int filled = sscanf(buffer, "%" SCNd64 "%n", &i, &len);
        if (len == cog_strlen(s)) {
            cog_push(cog_box_int(i));
            return NULL;
        }
    }
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_handle_int = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_int,
    doc_parser_internals,
};

cog_object* fn_parser_handle_float() {
    char buffer[32];
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        cog_string_to_cstring(s, buffer, sizeof(buffer));
        double i;
        int len = 0;
        int filled = sscanf(buffer, "%lg%n", &i, &len);
        if (len == cog_strlen(s)) {
            cog_push(cog_box_float(i));
            return NULL;
        }
    }
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_handle_float = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_float,
    doc_parser_internals,
};

cog_object* fn_parser_handle_bool() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    cog_push(cog_box_bool(cog_strcasecmp_c(s, "true") == 0));
    return NULL;
}
cog_modfunc fne_parser_handle_bool_true = {
    "true",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_bool,
    doc_parser_internals,
};
cog_modfunc fne_parser_handle_bool_false = {
    "false",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_bool,
    doc_parser_internals,
};

bool all_valid_for_ident(cog_object* string) {
    size_t len = cog_strlen(string);
    for (size_t i = 0; i < len; i++) {
        char ch = cog_nthchar(string, i);
        if (!(isalpha(ch) || isdigit(ch) || strchr("-?!'+/*>=<^.", ch))) return false;
    }
    return true;
}

cog_object* fn_parser_handle_symbols() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        char first = cog_nthchar(s, 0);
        if (first == '\\') {
            cog_string_delete_char(&s, 0);
            if (!all_valid_for_ident(s)) {
                cog_string_prepend_byte(&s, '\\');
            } else {
                cog_push(cog_sym(cog_make_identifier(s)));
                return NULL;
            }
        }
    }
    // defer to next stuff
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_handle_symbols = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_symbols,
    doc_parser_internals,
};

cog_object* fn_parser_discard_informal_syntax() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        char first = cog_nthchar(s, 0);
        if (tolower(first) == first && isalpha(first)) {
            // clear string to signal there is no token here
            s->stored_chars = 0;
            s->next = NULL;
        }
    }
    // defer to next stuff
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_discard_informal_syntax = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_discard_informal_syntax,
    doc_parser_internals,
};

cog_object* fn_parser_handle_identifiers() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        char first = cog_nthchar(s, 0);
        if (!isalpha(first) || (toupper(first) == first && all_valid_for_ident(s))) {
            // defined identifier
            cog_push(cog_make_identifier(s));
            return NULL;
        }
    }
    // defer to next stuff
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_handle_identifiers = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_identifiers,
    doc_parser_internals,
};

cog_object* fn_parser_handle_open_paren() {
    cog_object* cookie = cog_pop();
    cog_object* stream = cookie->next;
    cog_push(NULL); // the current block
    cog_push(NULL); // the current statement
    cog_object* cookie2 = stream;
    cog_push_to(&cookie2, cog_make_character(')'));
    cog_run_next(cog_make_identifier_c("[[Parser::ParseBlockLoop]]"), NULL, cookie2);
    run_nextitem_next(stream, cog_emptystring());
    return NULL;
}
cog_modfunc fne_parser_handle_open_paren = {
    "(",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_open_paren,
    doc_parser_internals
};

cog_obj_type ot_parser_sentinel = {"[[Parser::Object::Sentinel]]", cog_walk_only_next, NULL};

cog_object* make_sentinel(cog_object* ch) {
    cog_object* x = cog_make_obj(&ot_parser_sentinel);
    x->next = ch;
    return x;
}

cog_object* fn_parser_handle_close_paren_or_eof() {
    cog_object* cookie = cog_pop();
    cog_object* closer = cookie->data;
    if (closer->type == &ot_eof) goto yes;
    if (cog_strlen(closer) == 1 && strchr(")]}\xff", cog_nthchar(closer, 0))) goto yes;
    cog_push(cookie);
    return cog_not_implemented();

    yes:
    cog_push(make_sentinel(closer));
    return NULL;
}
cog_modfunc fne_parser_handle_close_paren_or_eof = {
    NULL,
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_close_paren_or_eof,
    doc_parser_internals
};

cog_object* fn_parser_handle_string() {
    cog_object* cookie = cog_pop();
    cog_object* stream = cookie->next;
    cog_object* buffer = cog_emptystring();
    cog_object* tail = buffer;
    char ch;
    for (;;) {
        ch = cog_getch(stream);
        if (ch == '"') break;
        if (ch == EOF || ch == '\n') goto unterminated;
        if (ch == '\\') {
            ch = cog_getch(stream);
            if (ch == EOF) goto unterminated;
            if (ch == 'x') {
                // \x escapes are not in cognac but github copilot
                // gave this to me for free and it works so why not
                char hex[3];
                hex[0] = cog_getch(stream);
                hex[1] = cog_getch(stream);
                hex[2] = 0;
                if (hex[0] == EOF || hex[1] == EOF) goto unterminated;
                if (!isxdigit(hex[0]) || !isxdigit(hex[1])) goto badescape;
                ch = strtol(hex, NULL, 16);
            } else {
                char ch2 = cog_unescape_char(ch);
                if (ch2 == ch) {
                    cog_string_append_byte(&tail, '\\');
                } else {
                    ch = ch2;
                }
            }
        }
        cog_string_append_byte(&tail, ch);
    }
    cog_push(buffer);
    return NULL;

    unterminated:
    cog_push(cog_string("unterminated string"));
    return cog_error();

    badescape:
    cog_push(cog_string("bad hex escape sequence in string"));
    return cog_error();
}
cog_modfunc fne_parser_handle_string = {
    "\"",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_string,
    doc_parser_internals
};

cog_object* fn_parser_handle_comments() {
    cog_object* cookie = cog_pop();
    cog_object* buffer = cookie->data;
    // there is no token here
    buffer->stored_chars = 0;
    buffer->next = NULL;
    cog_object* stream = cookie->next;
    bool isline = false;
    char ch = cog_getch(stream);
    if (ch == EOF) goto unterminated;
    if (ch == '~') isline =  true;
    for (;;) {
        ch = cog_getch(stream);
        if (!isline && ch == EOF) goto unterminated;
        if (!isline && ch == '~') break;
        if (isline && ch == '\n') break;
    }
    cog_push(cookie);
    return cog_not_implemented();

    unterminated:
    cog_push(cog_string("unterminated block comment"));
    return cog_error();
}
cog_modfunc fne_parser_handle_comments = {
    "~",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_comments,
    doc_parser_internals
};

cog_object* fn_parser_handle_def_and_let() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    bool is_def = !cog_strcasecmp_c(s, "def");
    cog_run_next(cog_make_identifier_c("[[Parser::TransformDefOrLet]]"), NULL, cog_box_bool(is_def));
    run_nextitem_next(cookie->next, cog_emptystring());
    return NULL;
}
cog_modfunc fne_parser_handle_def = {
    "def",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_def_and_let,
    doc_parser_internals
};
cog_modfunc fne_parser_handle_let = {
    "let",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_def_and_let,
    doc_parser_internals
};

cog_obj_type ot_def_or_let_special = {"[[Parser::Special::DefOrLet]]", cog_walk_only_next, NULL};
cog_obj_type ot_var = {"Var", cog_walk_only_next, NULL};

cog_object* make_def_or_let_special_obj(cog_object* what, bool is_def) {
    cog_object* obj = cog_make_obj(&ot_def_or_let_special);
    obj->as_int = is_def;
    obj->next = what;
    return obj;
}

cog_object* cog_make_var(cog_object* what) {
    cog_object* obj = cog_make_obj(&ot_var);
    obj->next = what;
    return obj;
}

cog_object* m_def_or_let_exec() {
    cog_object* self = cog_pop();
    cog_pop(); // ignore cookie
    bool is_def = self->as_int;
    cog_object* symbol = self->next;
    COG_ENSURE_N_ITEMS(1);
    cog_object* value = cog_pop();
    cog_defun(symbol, is_def ? value : cog_make_var(value));
    return NULL;
}
cog_object_method ome_def_or_let_exec = {&ot_def_or_let_special, COG_M_EXEC, m_def_or_let_exec};
cog_object_method ome_def_or_let_hash = {&ot_def_or_let_special, COG_M_HASH, cog_not_implemented};

cog_object* m_def_or_let_show() {
    cog_object* self = cog_pop();
    cog_pop(); // ignore readably
    bool is_def = self->as_int;
    cog_object* symbol = self->next;
    cog_push(cog_sprintf("<%s %O>", is_def ? "Def" : "Let", symbol));
    return NULL;
}
cog_object_method ome_def_or_let_show = {&ot_def_or_let_special, COG_M_SHOW, m_def_or_let_show};

cog_object* m_var_run_self() {
    cog_object* self = cog_pop();
    cog_pop(); // ignore cookie
    cog_push(self->next);
    return NULL;
}
cog_object_method ome_var_exec = {&ot_var, COG_M_EXEC, m_var_run_self};
cog_object_method ome_var_hash = {&ot_var, COG_M_HASH, cog_not_implemented};



cog_object* fn_parser_transform_def_or_let() {
    cog_object* cookie = cog_pop();
    bool is_def = cookie->as_int;
    cog_object* what = cog_pop();
    if (!what || what->type != &cog_ot_identifier) goto error;
    cog_push(make_def_or_let_special_obj(what, is_def));
    return NULL;

    error:
    cog_push(cog_sprintf("expected identifier after %s not %O", is_def ? "Def" : "Let", what));
    return cog_error();
}
cog_modfunc fne_parser_transform_def_or_let = {
    "[[Parser::TransformDefOrLet]]",
    COG_COOKIEFUNC,
    fn_parser_transform_def_or_let,
    doc_parser_internals
};

void handle_end_of_statement() {
    cog_object* statement = cog_pop();
    cog_object* block_contents = cog_pop();
    cog_list_splice(&block_contents, statement);
    cog_push(block_contents);
    cog_push(NULL); // next statement list
}

void handle_end_of_block() {
    handle_end_of_statement();
    cog_pop(); // throw out empty new statement;
    cog_object* commands = cog_pop();
    cog_push(cog_make_block(commands));
}

void handle_item(cog_object* ijp) {
    cog_object* statement = cog_pop();
    cog_push_to(&statement, ijp);
    cog_push(statement);
}

cog_object* fn_parser_handle_semicolon() {
    cog_object* cookie = cog_pop();
    cog_object* s = cookie->data;
    if (s->type == &cog_ot_string) {
        s->stored_chars = 0;
        s->next = NULL;
    }
    handle_end_of_statement();
    cog_push(cookie);
    return cog_not_implemented();
}
cog_modfunc fne_parser_handle_semicolon = {
    ";",
    COG_PARSE_TOKEN_HANDLER,
    fn_parser_handle_semicolon,
    doc_parser_internals
};

cog_object* fn_parser_parse_block_loop() {
    cog_object* cookie = cog_pop();
    cog_object* stopwhen = cookie->data;
    cog_object* stream = cookie->next;
    cog_object* ijp = cog_pop();
    if (!ijp || ijp->type != &ot_parser_sentinel) goto next;
    if (ijp->next->type == &ot_eof && stopwhen->type == &ot_eof) goto stop;
    if (ijp->next->type == &cog_ot_string && stopwhen->type == &cog_ot_string && !cog_strcmp(ijp->next, stopwhen)) goto stop;
    // else it is an error
    cog_push(ijp->next->type == &cog_ot_string ? cog_sprintf("unexpected %O", ijp->next) : cog_string("unexpected EOF"));
    return cog_error();

    next:
    handle_item(ijp);
    cog_run_next(cog_make_identifier_c("[[Parser::ParseBlockLoop]]"), NULL, cookie);
    run_nextitem_next(stream, cog_emptystring());
    return NULL;

    stop:
    handle_end_of_block();
    return NULL;
}
cog_modfunc fne_parser_parse_block_loop = {
    "[[Parser::ParseBlockLoop]]",
    COG_COOKIEFUNC,
    fn_parser_parse_block_loop,
    doc_parser_internals
};

cog_object* fn_parse() {
    cog_object* stream = cog_pop();
    if (stream == NULL) {
        cog_push(NULL);
        return NULL;
    }
    if (stream->type == &cog_ot_string) {
        stream = cog_iostring_wrap(stream);
    }
    cog_object* cookie2 = stream;
    cog_push_to(&cookie2, cog_eof());
    cog_run_next(cog_make_identifier_c("[[Parser::ParseBlockLoop]]"), NULL, cookie2);
    run_nextitem_next(stream, cog_emptystring());
    cog_push(NULL);
    cog_push(NULL); // for block parsing and stuff
    return NULL;
}
cog_modfunc fne_parse = {
    "Parse",
    COG_FUNC,
    fn_parse,
    "Parse the next item from the stream on the stack "
    "into a Cognate data structure."
};

// MARK: BUILTIN FUNCTIONS

cog_object* fn_empty() {
    cog_push(NULL);
    return NULL;
}
cog_modfunc fne_empty = {
    "Empty",
    COG_FUNC,
    fn_empty,
    "Return an empty list."
};

#define GET_TYPENAME_STRING(obj) (obj && obj->type ? obj->type->name : "NULL")
#define _NUMBERBODY(op, either_float_type, both_ints_type, both_ints_cast) \
    COG_ENSURE_N_ITEMS(2); \
    cog_object* a = cog_pop(); \
    cog_object* b = cog_pop(); \
    if (a && b) { \
        if ((a->type == &cog_ot_int || a->type == &cog_ot_float) && (b->type == &cog_ot_int || b->type == &cog_ot_float)) { \
            if (a->type == &cog_ot_int && b->type == &cog_ot_int) { \
                cog_push(cog_box_##both_ints_type((both_ints_cast b->as_int) op (both_ints_cast a->as_int))); \
            } else { \
                double a_val = (a->type == &cog_ot_int) ? (double)a->as_int : a->as_float; \
                double b_val = (b->type == &cog_ot_int) ? (double)b->as_int : b->as_float; \
                cog_push(cog_box_##either_float_type(b_val op a_val)); \
            } \
            return NULL; \
        } \
    } \
    cog_push(cog_sprintf("Can't apply operator %s to %s and %s", #op, \
        GET_TYPENAME_STRING(a), GET_TYPENAME_STRING(b))); \
    return cog_error();

cog_object* fn_plus() { _NUMBERBODY(+, float, int, ) }
cog_object* fn_minus() { _NUMBERBODY(-, float, int, ) }
cog_object* fn_times() { _NUMBERBODY(*, float, int, ) }
cog_object* fn_divide() { _NUMBERBODY(/, float, float, (double)) }
cog_object* fn_less() { _NUMBERBODY(<, bool, bool, ) }
cog_object* fn_greater() { _NUMBERBODY(>, bool, bool, ) }
cog_object* fn_lesseq() { _NUMBERBODY(<=, bool, bool, ) }
cog_object* fn_greatereq() { _NUMBERBODY(>=, bool, bool, ) } // cSpell: ignore greatereq
cog_object* fn_pow() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* a = cog_pop();
    cog_object* b = cog_pop();
    double a_val, b_val;
    COG_GET_NUMBER(a, a_val);
    COG_GET_NUMBER(b, b_val);
    cog_push(cog_box_float(pow(b_val, a_val)));
    return NULL;
}

cog_modfunc fne_plus = {"+", COG_FUNC, fn_plus, "Add two numbers."};
cog_modfunc fne_minus = {"-", COG_FUNC, fn_minus, "Subtract two numbers."};
cog_modfunc fne_times = {"*", COG_FUNC, fn_times, "Multiply two numbers."};
cog_modfunc fne_divide = {"/", COG_FUNC, fn_divide, "Divide two numbers."};
cog_modfunc fne_less = {"<", COG_FUNC, fn_less, "Check if a is less than b."};
cog_modfunc fne_greater = {">", COG_FUNC, fn_greater, "Check if a is greater than b."};
cog_modfunc fne_lesseq = {"<=", COG_FUNC, fn_lesseq, "Check if a is less than or equal to b."};
cog_modfunc fne_greatereq = {">=", COG_FUNC, fn_greatereq, "Check if a is greater than or equal to b."};
cog_modfunc fne_pow = {"^", COG_FUNC, fn_pow, "Get the power of a to b."};

cog_object* fn_eq() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* a = cog_pop();
    cog_object* b = cog_pop();
    bool result = false;
    if (a && b) {
        if (a->type == b->type) {
            if (a->type == &cog_ot_int) {
                result = (a->as_int == b->as_int);
            } else if (a->type == &cog_ot_float) {
                result = (a->as_float == b->as_float);
            } else if (a->type == &cog_ot_bool) {
                result = (a->as_int == b->as_int);
            } else if (a->type == &cog_ot_identifier) {
                result = cog_same_identifiers(a, b);
            } else if (a->type == &cog_ot_symbol) {
                result = cog_same_identifiers(a->next, b->next);
            } else if (a->type == &cog_ot_string) {
                result = (cog_strcmp(a, b) == 0);
            } else if (a->type == &cog_ot_list) {
                cog_push(b);
                cog_push(a);
                cog_run_next(cog_make_identifier_c("SameLists"), NULL, NULL);
                return NULL;
            } else {
                result = (a == b);
            }
        } else if (a->type == &cog_ot_int && b->type == &cog_ot_float) {
            result = (a->as_int == b->as_float);
        } else if (a->type == &cog_ot_float && b->type == &cog_ot_int) {
            result = (a->as_float == b->as_int);
        } else {
            result = false;
        }
    } else if (a == b) {
        result = true;
    } else {
        result = false;
    }
    cog_push(cog_box_bool(result));
    return NULL;
}
cog_modfunc fne_eq = {"==", COG_FUNC, fn_eq, "Check if two objects are equal."};

cog_object* fn_if() {
    COG_ENSURE_N_ITEMS(3);
    cog_object* cond = cog_pop();
    cog_object* iftrue = cog_pop();
    cog_object* iffalse = cog_pop();
    COG_ENSURE_TYPE(cond, &cog_ot_bool);
    cog_push(cond->as_int ? iftrue : iffalse);
    return NULL;
}
cog_modfunc fne_if = {"If", COG_FUNC, fn_if, "If cond is true, return iftrue, else return iffalse."};

cog_object* fn_print() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* obj = cog_pop();
    cog_printf(obj && obj->type == &cog_ot_string ? "%#O\n" : "%O\n", obj);
    return NULL;
}
cog_modfunc fne_print = {"Print", COG_FUNC, fn_print, "Print an object to stdout, with a newline."};

cog_object* fn_put() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* obj = cog_pop();
    cog_printf(obj && obj->type == &cog_ot_string ? "%#O" : "%O", obj);
    return NULL;
}
cog_modfunc fne_put = {"Put", COG_FUNC, fn_put, "Print an object to stdout, without a newline."};

cog_object* fn_do() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* obj = cog_pop();
    cog_run_next(obj, NULL, NULL);
    return NULL;
}
cog_modfunc fne_do = {"Do", COG_FUNC, fn_do, "Run the item on the stack."};

cog_object* fn_random() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* low = cog_pop();
    cog_object* high = cog_pop();
    double n_low, n_high;
    COG_GET_NUMBER(low, n_low);
    COG_GET_NUMBER(high, n_high);
    if (n_high > n_low) {
        double tmp = n_high;
        n_high = n_low;
        n_low = tmp;
    }
    double diff = n_high - n_low;
    double x = (double)rand() / (double)(RAND_MAX / diff) + n_low;
    cog_push(cog_box_float(x));
    return NULL;
}
cog_modfunc fne_random = {"Random", COG_FUNC, fn_random, "Return a random number between low and high."};

cog_object* fn_modulo() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* a = cog_pop();
    cog_object* b = cog_pop();
    if (a && b && a->type == &cog_ot_int && b->type == &cog_ot_int) {
        cog_push(cog_box_int(b->as_int % a->as_int));
    } else {
        double a_val, b_val;
        COG_GET_NUMBER(a, a_val);
        COG_GET_NUMBER(b, b_val);
        cog_push(cog_box_float(fmod(b_val, a_val)));
    }
    return NULL;
}
cog_modfunc fne_modulo = {"Modulo", COG_FUNC, fn_modulo, "Return the modulo of two numbers."};

cog_object* fn_sqrt() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    double a_val;
    COG_GET_NUMBER(a, a_val);
    cog_push(cog_box_float(sqrt(a_val)));
    return NULL;
}
cog_modfunc fne_sqrt = {"Sqrt", COG_FUNC, fn_sqrt, "Return the square root of a number."};

#define _BOOLBODY(op) \
    COG_ENSURE_N_ITEMS(2); \
    cog_object* a = cog_pop(); \
    cog_object* b = cog_pop(); \
    COG_ENSURE_TYPE(a, &cog_ot_bool); \
    COG_ENSURE_TYPE(b, &cog_ot_bool); \
    cog_push(cog_box_bool(a->as_int op b->as_int)); \
    return NULL; \

cog_object* fn_or() { _BOOLBODY(||) }
cog_object* fn_and() { _BOOLBODY(&&) }
cog_object* fn_xor() { _BOOLBODY(^) }
cog_modfunc fne_or = {"Or", COG_FUNC, fn_or, "Return the logical OR of two booleans."};
cog_modfunc fne_and = {"And", COG_FUNC, fn_and, "Return the logical AND of two booleans."};
cog_modfunc fne_xor = {"Xor", COG_FUNC, fn_xor, "Return the logical XOR of two booleans."};

cog_object* fn_not() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    COG_ENSURE_TYPE(a, &cog_ot_bool);
    cog_push(cog_box_bool(!a->as_int));
    return NULL;
}
cog_modfunc fne_not = {"Not", COG_FUNC, fn_not, "Return the logical NOT of a boolean."};

cog_object* fn_is_number() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    cog_push(cog_box_bool(a->type == &cog_ot_int || a->type == &cog_ot_float));
    return NULL;
}
cog_modfunc fne_is_number = {"Number?", COG_FUNC, fn_is_number, "Return true if the object is a number (integer or float)."};

#define _TYPEP_BODY(f, typeobj) \
    COG_ENSURE_N_ITEMS(1); \
    cog_object* a = cog_pop(); \
    cog_push(cog_box_bool(f a->type == typeobj)); \
    return NULL;

cog_object* fn_is_symbol() { _TYPEP_BODY(,&cog_ot_symbol) }
cog_object* fn_is_integer() { _TYPEP_BODY(,&cog_ot_int || (a->type == &cog_ot_float && a->as_float == floor(a->as_float))) }
cog_object* fn_is_list() { _TYPEP_BODY(!a ||, &cog_ot_list) }
cog_object* fn_is_string() { _TYPEP_BODY(,&cog_ot_string) }
cog_object* fn_is_block() { _TYPEP_BODY(,&ot_closure) }
cog_object* fn_is_boolean() { _TYPEP_BODY(,&cog_ot_bool) }
cog_modfunc fne_is_symbol = {"Symbol?", COG_FUNC, fn_is_symbol, "Return true if the object is a symbol."};
cog_modfunc fne_is_integer = {"Integer?", COG_FUNC, fn_is_integer, "Return true if the object is an integer."};
cog_modfunc fne_is_list = {"List?", COG_FUNC, fn_is_list, "Return true if the object is a list."};
cog_modfunc fne_is_string = {"String?", COG_FUNC, fn_is_string, "Return true if the object is a string."};
cog_modfunc fne_is_block = {"Block?", COG_FUNC, fn_is_block, "Return true if the object is a block."};
cog_modfunc fne_is_boolean = {"Boolean?", COG_FUNC, fn_is_boolean, "Return true if the object is a boolean."};

cog_object* fn_is_zero() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    if (a->type == &cog_ot_int) {
        cog_push(cog_box_bool(a->as_int == 0));
    } else if (a->type == &cog_ot_float) {
        cog_push(cog_box_bool(a->as_float == 0));
    } else {
        cog_push(cog_box_bool(false));
    }
    return NULL;
}
cog_modfunc fne_is_zero = {"Zero?", COG_FUNC, fn_is_zero, "Return true if the object is a number and is zero."};

cog_object* fn_is_io() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    cog_push(cog_emptystring());
    cog_object* res = cog_run_well_known(a, COG_SM_PUTS);
    cog_push(cog_box_bool(!cog_same_identifiers(res, cog_not_implemented())));
    return NULL;
}
cog_modfunc fne_is_io = {"IO?", COG_FUNC, fn_is_io, "Return true if the object is an IO object; i.e. it responds to Write."};

#define _TYPE_ASSERTION_BODY(typeobj, texpr) \
    COG_ENSURE_N_ITEMS(1); \
    cog_object* a = cog_pop(); \
    if (!a || a->type != texpr) COG_ENSURE_TYPE(a, typeobj); \
    cog_push(a); \
    return NULL;
cog_object* fn_assert_number() { _TYPE_ASSERTION_BODY(&cog_ot_float, &cog_ot_int || a->type == &cog_ot_float) }
cog_object* fn_assert_symbol() { _TYPE_ASSERTION_BODY(&cog_ot_symbol, &cog_ot_symbol) }
cog_object* fn_assert_string() { _TYPE_ASSERTION_BODY(&cog_ot_string, &cog_ot_string) }
cog_object* fn_assert_block() { _TYPE_ASSERTION_BODY(&ot_closure, &ot_closure) }
cog_object* fn_assert_boolean() { _TYPE_ASSERTION_BODY(&cog_ot_bool, &cog_ot_bool) }
cog_modfunc fne_assert_number = {"Number!", COG_FUNC, fn_assert_number, "Assert that the object is a number."};
cog_modfunc fne_assert_symbol = {"Symbol!", COG_FUNC, fn_assert_symbol, "Assert that the object is a symbol."};
cog_modfunc fne_assert_string = {"String!", COG_FUNC, fn_assert_string, "Assert that the object is a string."};
cog_modfunc fne_assert_block = {"Block!", COG_FUNC, fn_assert_block, "Assert that the object is a block."};
cog_modfunc fne_assert_boolean = {"Boolean!", COG_FUNC, fn_assert_boolean, "Assert that the object is a boolean."};

cog_object* fn_assert_list() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    COG_ENSURE_LIST(a);
    cog_push(a);
    return NULL;
}
cog_modfunc fne_assert_list = {"List!", COG_FUNC, fn_assert_list, "Assert that the object is a list."};

cog_object* fn_assert_io() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    cog_push(cog_emptystring());
    cog_object* res = cog_run_well_known(a, COG_SM_PUTS);
    if (cog_same_identifiers(res, cog_not_implemented())) {
        cog_push(cog_sprintf("expected IO object, got %s", GET_TYPENAME_STRING(a)));
        return cog_error();
    }
    cog_push(a);
    return NULL;
}
cog_modfunc fne_assert_io = {"IO!", COG_FUNC, fn_assert_io, "Assert that the object is an IO object."};

cog_object* fn_first() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    if (a && a->type == &cog_ot_string) {
        if (cog_strlen(a) == 0) COG_RETURN_ERROR(cog_string("tried to get First of an empty string"));
        cog_push(cog_make_character(cog_nthchar(a, 0)));
        return NULL;
    }
    COG_ENSURE_LIST(a);
    if (!a) COG_RETURN_ERROR(cog_string("tried to get First of an empty list"));
    cog_push(a->data);
    return NULL;
}
cog_modfunc fne_first = {"First", COG_FUNC, fn_first, "Return the first element of a list."};

cog_object* fn_rest() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    if (a && a->type == &cog_ot_string) {
        if (cog_strlen(a) == 0) COG_RETURN_ERROR(cog_string("tried to get Rest of an empty string"));
        cog_object* dup = cog_strdup(a);
        cog_string_delete_char(&dup, 0);
        cog_push(dup);
        return NULL;
    }
    COG_ENSURE_LIST(a);
    if (!a) COG_RETURN_ERROR(cog_string("tried to get Rest of an empty list"));
    cog_push(a->next);
    return NULL;
}
cog_modfunc fne_rest = {"Rest", COG_FUNC, fn_rest, "Return the rest of a list."};

cog_object* fn_push() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* a = cog_pop();
    cog_object* b = cog_pop();
    COG_ENSURE_LIST(b);
    cog_push_to(&b, a);
    cog_push(b);
    return NULL;
}
cog_modfunc fne_push = {"Push", COG_FUNC, fn_push, "Push an item onto a list and return the new list."};

cog_object* fn_is_empty() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    if (a && a->type == &cog_ot_string) {
        cog_push(cog_box_bool(cog_strlen(a) == 0));
    } else {
        COG_ENSURE_LIST(a);
        cog_push(cog_box_bool(!a));
    }
    return NULL;
}
cog_modfunc fne_is_empty = {"Empty?", COG_FUNC, fn_is_empty, "Return true if the list is empty."};

cog_object* fn_append() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* a = cog_pop();
    cog_object* b = cog_pop();
    if (a && b && a->type == &cog_ot_string && b->type == &cog_ot_string) {
        cog_object* ba = cog_strappend(b, a);
        // char cha = cog_nthchar(a, 0);
        // char chb = cog_nthchar(ba, cog_strlen(b));
        // cog_printf("DEBUG: a=%O, b=%O, cha=%c, chb=%c, ba=%O\n", a, b, cha, chb, ba);
        // assert(cha == chb);
        cog_push(ba);
        return NULL;
    }
    COG_ENSURE_LIST(a);
    COG_ENSURE_LIST(b);
    cog_list_splice(&b, a);
    cog_push(b);
    return NULL;
}
cog_modfunc fne_append = {"Append", COG_FUNC, fn_append, "Append two lists or strings."};

cog_object* fn_substring() {
    COG_ENSURE_N_ITEMS(3);
    cog_object* end = cog_pop();
    cog_object* start = cog_pop();
    cog_object* a = cog_pop();
    COG_ENSURE_TYPE(a, &cog_ot_string);
    COG_ENSURE_TYPE(start, &cog_ot_int);
    COG_ENSURE_TYPE(end, &cog_ot_int);
    cog_push(cog_substring(a, start->as_int, end->as_int));
    return NULL;
}
cog_modfunc fne_substring = {"Substring", COG_FUNC, fn_substring, "Return a substring of a string."};

cog_object* fn_ordinal() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    COG_ENSURE_TYPE(a, &cog_ot_string);
    char b[MB_CUR_MAX + 1];
    cog_string_to_cstring(a, b, MB_CUR_MAX);
    if (!b[0])
		COG_RETURN_ERROR(cog_string("Gave empty string to Ordinal"));
	wchar_t chr = 0;
    mbtowc(NULL, NULL, 0); // reset the conversion state
	mbtowc(&chr, b, MB_CUR_MAX);
    cog_push(cog_box_int(chr));
    return NULL;
}
cog_modfunc fne_ordinal = {"Ordinal", COG_FUNC, fn_ordinal, "Return the ordinal of a Unicode character (a one-character string)."};

cog_object* fn_character() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    if (!a || a->type != &cog_ot_int) {
        if (!a || a->type != &cog_ot_float || floor(a->as_float) != a->as_float)
            COG_ENSURE_TYPE(a, &cog_ot_int);
    }
    wchar_t ord = (wchar_t)(a->type == &cog_ot_int ? a->as_int : (int64_t)a->as_float);
    mbtowc(NULL, NULL, 0); // reset the conversion state
    char b[MB_CUR_MAX+1];
    memset(b, 0, MB_CUR_MAX + 1);
    size_t len = wctomb(b, ord);
    if (len == (size_t)-1)
        COG_RETURN_ERROR(cog_sprintf("Invalid ordinal %O", a));
    cog_push(cog_string(b));
    return NULL;
}
cog_modfunc fne_character = {"Character", COG_FUNC, fn_character, "Return the character of a Unicode ordinal."};

cog_object* fn_split() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* sep = cog_pop();
    cog_object* a = cog_pop();
    COG_ENSURE_TYPE(a, &cog_ot_string);
    COG_ENSURE_TYPE(sep, &cog_ot_string);
    cog_object* list = NULL;
    size_t startpos = 0;
    size_t seplen = cog_strlen(sep);
    size_t len = cog_strlen(a);
    for (size_t i = 0; i < len; i++) {
        if (!cog_strncmp(cog_substring(a, i, len), sep, seplen)) {
            cog_push_to(&list, cog_substring(a, startpos, i));
            startpos = i + seplen;
            i += seplen - 1;
        }
    }
    if (startpos < len)
        cog_push_to(&list, cog_substring(a, startpos, len));
    cog_reverse_list_inplace(&list);
    cog_push(list);
    return NULL;
}
cog_modfunc fne_split = {"Split", COG_FUNC, fn_split, "Split a string into a list of substrings."};

#define _UPPERLOWERBODY(ulfunc) \
    COG_ENSURE_N_ITEMS(1); \
    cog_object* str = cog_pop(); \
    COG_ENSURE_TYPE(str, &cog_ot_string); \
    cog_object* result = cog_emptystring(); \
    cog_object* tail = result; \
    char buffer[MB_CUR_MAX + 1]; \
    int i = 0; \
    while (str) { \
        memset(buffer, 0, sizeof(buffer)); \
        int len = min(MB_CUR_MAX, str->stored_chars - i); \
        strncpy(buffer, &str->as_chars[i], len); \
        /* If the current chunk has less than MB_CUR_MAX characters, */ \
        /* copy remaining characters from the next chunk             */ \
        if (len < MB_CUR_MAX && str->next) { \
            int remaining = MB_CUR_MAX - len; \
            int next_len = min(remaining, str->next->stored_chars); \
            strncat(buffer, str->next->as_chars, next_len); \
        } \
        wchar_t wc; \
        len = mblen(buffer, MB_CUR_MAX); \
        mbtowc(&wc, buffer, len); \
        wc = ulfunc(wc); \
        memset(buffer, 0, sizeof(buffer)); \
        wctomb(buffer, wc); \
        for (int j = 0; j < strlen(buffer); j++) { \
            cog_string_append_byte(&tail, buffer[j]); \
        } \
        i += len; \
        while (str && i >= str->stored_chars) { \
            i -= str->stored_chars; \
            str = str->next; \
        } \
    } \
    cog_push(result); \
    return NULL; \

cog_object* fn_lowercase() { _UPPERLOWERBODY(towlower) }
cog_object* fn_uppercase() { _UPPERLOWERBODY(towupper) }
cog_modfunc fne_lowercase = {"Lowercase", COG_FUNC, fn_lowercase, "Converts a string to lower case."};
cog_modfunc fne_uppercase = {"Uppercase", COG_FUNC, fn_uppercase, "Converts a string to upper case."};

#define _ONEFUNNUMBODY(ffn, ifn) \
    COG_ENSURE_N_ITEMS(1); \
    cog_object* a = cog_pop(); \
    if (a && a->type == &cog_ot_int) { \
        cog_push(cog_box_int(ifn(a->as_int))); \
        return NULL; \
    } \
    COG_ENSURE_TYPE(a, &cog_ot_float); \
    cog_push(cog_box_float(ffn(a->as_float))); \
    return NULL;

cog_object* fn_floor() { _ONEFUNNUMBODY(floor,) }
cog_object* fn_round() { _ONEFUNNUMBODY(round,) }
cog_object* fn_ceil() { _ONEFUNNUMBODY(ceil,) }
cog_object* fn_abs()  { _ONEFUNNUMBODY(fabs, llabs) }
cog_modfunc fne_floor = {"Floor", COG_FUNC, fn_floor, "Return the floor of a number."};
cog_modfunc fne_round = {"Round", COG_FUNC, fn_round, "Return the rounded number."};
cog_modfunc fne_ceil = {"Ceiling", COG_FUNC, fn_ceil, "Return the ceiling of a number."};
cog_modfunc fne_abs = {"Abs", COG_FUNC, fn_abs, "Return the absolute value of a number."};

cog_object* fn_error() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    COG_ENSURE_TYPE(a, &cog_ot_string);
    cog_push(a);
    return cog_error();
}
cog_modfunc fne_error = {"Error", COG_FUNC, fn_error, "Raise an error with a message."};

cog_object* fn_list() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* block = cog_pop();
    COG_ENSURE_TYPE(block, &ot_closure);
    cog_run_next(cog_make_identifier_c("[[List::Finish]]"), NULL, COG_GLOBALS.stack);
    cog_run_next(block, NULL, NULL);
    COG_GLOBALS.stack = NULL;
    return NULL;
}
cog_modfunc fne_list = {"List", COG_FUNC, fn_list, "Create a list by using the stack created by a block."};

cog_object* fn_list_finish() {
    cog_object* old_stack = cog_pop();
    cog_object* list = COG_GLOBALS.stack;
    COG_GLOBALS.stack = old_stack;
    cog_push(list);
    return NULL;
}
cog_modfunc fne_list_finish = {"[[List::Finish]]", COG_COOKIEFUNC, fn_list_finish, NULL};

cog_object* fn_number() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    COG_ENSURE_TYPE(a, &cog_ot_string);
    char b[64];
    cog_string_to_cstring(a, b, sizeof(b));
    int64_t i = 0;
    double f = 0;
    if (sscanf(b, "%lld", &i) == 1) {
        cog_push(cog_box_int(i));
    } else if (sscanf(b, "%lf", &f) == 1) {
        cog_push(cog_box_float(f));
    } else {
        COG_RETURN_ERROR(cog_sprintf("Can't convert %O to a number", a));
    }
    return NULL;
}
cog_modfunc fne_number = {"Number", COG_FUNC, fn_number, "Convert a string to a number."};

cog_object* fn_wait() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    double duration;
    COG_GET_NUMBER(a, duration);
    usleep(duration * 1000000);
    return NULL;
}
cog_modfunc fne_wait = {"Wait", COG_FUNC, fn_wait, "Sleep for a number of seconds."};

cog_object* fn_stop() {
    COG_RETURN_ERROR(NULL);
}
cog_modfunc fne_stop = {"Stop", COG_FUNC, fn_stop, "Stop the program."};

cog_object* fn_show() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* a = cog_pop();
    cog_push(cog_sprintf("%#O", a));
    return NULL;
}
cog_modfunc fne_show = {"Show", COG_FUNC, fn_show, "Turn an object into its human-readable string representation."};

cog_object* fn_stack() {
    cog_push(COG_GLOBALS.stack);
    return NULL;
}
cog_modfunc fne_stack = {"Stack", COG_FUNC, fn_stack, "Push the stack to itself."};

cog_object* fn_clear() {
    COG_GLOBALS.stack = NULL;
    return NULL;
}
cog_modfunc fne_clear = {"Clear", COG_FUNC, fn_clear, "Empty everything from the stack."};

cog_obj_type ot_box = {"Box", cog_walk_only_next, NULL};

static cog_object* _box(cog_object* what) {
    cog_object* b = cog_make_obj(&ot_box);
    b->next = what;
    return b;
}

cog_object* fn_box() {
    COG_ENSURE_N_ITEMS(1);
    cog_push(_box(cog_pop()));
    return NULL;
}
cog_modfunc fne_box = {"Box", COG_FUNC, fn_box, "Puts the object into a box, which is a mutable pointer to it."};

cog_object* fn_unbox() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* the_box = cog_pop();
    COG_ENSURE_TYPE(the_box, &ot_box);
    cog_push(the_box->next);
    return NULL;
}
cog_modfunc fne_unbox = {"Unbox", COG_FUNC, fn_unbox, "Reverse of Box. Pulls the pointed-to object out of the mutable pointer."};

cog_object* fn_set() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* the_box = cog_pop();
    cog_object* the_item = cog_pop();
    COG_ENSURE_TYPE(the_box, &ot_box);
    the_box->next = the_item;
    return NULL;
}
cog_modfunc fne_set = {"Set", COG_FUNC, fn_set, "Mutates a box in-place by changing the object it points to."};

#define _TRIG_FUNC(f1, f2) \
    COG_ENSURE_N_ITEMS(1); \
    cog_object* obj = cog_pop(); \
    double n = 0; \
    COG_GET_NUMBER(obj, n); \
    cog_push(cog_box_float(f1(f2(n)))); \
    return NULL;

double deg2rad(double deg) { return deg * M_PI / 180.; }
double rad2deg(double rad) { return rad * 180. / M_PI; }

cog_object* fn_sind() { _TRIG_FUNC(sin, deg2rad) }
cog_object* fn_cosd() { _TRIG_FUNC(cos, deg2rad) }
cog_object* fn_tand() { _TRIG_FUNC(tan, deg2rad) }
cog_object* fn_sin() { _TRIG_FUNC(sin,) }
cog_object* fn_cos() { _TRIG_FUNC(cos,) }
cog_object* fn_tan() { _TRIG_FUNC(tan,) }
cog_object* fn_exp() { _TRIG_FUNC(exp,) }
cog_object* fn_ln() { _TRIG_FUNC(log,) }
cog_object* fn_asind() { _TRIG_FUNC(rad2deg, asin) }
cog_object* fn_acosd() { _TRIG_FUNC(rad2deg, acos) }
cog_object* fn_atand() { _TRIG_FUNC(rad2deg, atan) }
cog_object* fn_asin() { _TRIG_FUNC(asin,) }
cog_object* fn_acos() { _TRIG_FUNC(acos,) }
cog_object* fn_atan() { _TRIG_FUNC(atan,) }
cog_object* fn_sinhd() { _TRIG_FUNC(sinh, deg2rad) }
cog_object* fn_coshd() { _TRIG_FUNC(cosh, deg2rad) }
cog_object* fn_tanhd() { _TRIG_FUNC(tanh, deg2rad) }
cog_object* fn_sinh() { _TRIG_FUNC(sinh,) }
cog_object* fn_cosh() { _TRIG_FUNC(cosh,) }
cog_object* fn_tanh() { _TRIG_FUNC(tanh,) }
cog_modfunc fne_sind = {"Sind", COG_FUNC, fn_sind, "Return the sine of the angle, which is expressed in degrees."};
cog_modfunc fne_cosd = {"Cosd", COG_FUNC, fn_cosd, "Return the cosine of the angle, which is expressed in degrees."};
cog_modfunc fne_tand = {"Tand", COG_FUNC, fn_tand, "Return the tangent of the angle, which is expressed in degrees."};
cog_modfunc fne_sin = {"Sin", COG_FUNC, fn_sin, "Return the sine of the angle, which is expressed in radians."};
cog_modfunc fne_cos = {"Cos", COG_FUNC, fn_cos, "Return the cosine of the angle, which is expressed in radians."};
cog_modfunc fne_tan = {"Tan", COG_FUNC, fn_tan, "Return the tangent of the angle, which is expressed in radians."};
cog_modfunc fne_exp = {"Exp", COG_FUNC, fn_exp, "Return the base-e exponential of the number."};
cog_modfunc fne_ln = {"Ln", COG_FUNC, fn_ln, "Return the natural (base-e) logarithm of the number."};
cog_modfunc fne_asind = {"Asind", COG_FUNC, fn_asind, "Return the inverse sine of the value, in degrees."};
cog_modfunc fne_acosd = {"Acosd", COG_FUNC, fn_acosd, "Return the inverse cosine of the value, in degrees."};
cog_modfunc fne_atand = {"Atand", COG_FUNC, fn_atand, "Return the inverse tangent of the value, in degrees."};
cog_modfunc fne_asin = {"Asin", COG_FUNC, fn_asin, "Return the inverse sine of the value, in radians."};
cog_modfunc fne_acos = {"Acos", COG_FUNC, fn_acos, "Return the inverse cosine of the value, in radians."};
cog_modfunc fne_atan = {"Atan", COG_FUNC, fn_atan, "Return the inverse tangent of the value, in radians."};
cog_modfunc fne_sinhd = {"Sinhd", COG_FUNC, fn_sinhd, "Return the hyperbolic sine of the angle, which is expressed in degrees."};
cog_modfunc fne_coshd = {"Coshd", COG_FUNC, fn_coshd, "Return the hyperbolic cosine of the angle, which is expressed in degrees."};
cog_modfunc fne_tanhd = {"Tanhd", COG_FUNC, fn_tanhd, "Return the hyperbolic tangent of the angle, which is expressed in degrees."};
cog_modfunc fne_sinh = {"Sinh", COG_FUNC, fn_sinh, "Return the hyperbolic sine of the angle, which is expressed in radians."};
cog_modfunc fne_cosh = {"Cosh", COG_FUNC, fn_cosh, "Return the hyperbolic cosine of the angle, which is expressed in radians."};
cog_modfunc fne_tanh = {"Tanh", COG_FUNC, fn_tanh, "Return the hyperbolic tangent of the angle, which is expressed in radians."};

cog_obj_type cog_ot_continuation = {"Continuation", cog_walk_both, NULL};

cog_object* cog_make_continuation() {
    cog_object* c = cog_make_obj(&cog_ot_continuation);
    c->data = COG_GLOBALS.stack;
    c->next = cog_make_obj(&cog_ot_list);
    c->next->data = COG_GLOBALS.command_queue;
    c->next->next = COG_GLOBALS.scopes;
    return c;
}

cog_object* m_continuation_exec() {
    cog_object* self = cog_pop();
    cog_pop(); // ignore cookie
    cog_object* contval = cog_pop();
    cog_object* old_stack = self->data;
    cog_object* old_command_queue = self->next->data;
    cog_object* old_scopes = self->next->next;
    // TODO: get displaced enter and exit handlers and queue them to be run
    // TODO: this would mean continuations don't have to save the scopes because it gets saved
    // TODO: on the command queue cookie of closures' enter handlers
    COG_GLOBALS.stack = old_stack;
    COG_GLOBALS.command_queue = old_command_queue;
    COG_GLOBALS.scopes = old_scopes;
    cog_push(contval);
    return NULL;
}
cog_object_method ome_continuation_exec = {&cog_ot_continuation, COG_M_EXEC, m_continuation_exec};

cog_object* fn_begin() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* block = cog_pop();
    COG_ENSURE_TYPE(block, &ot_closure);
    cog_push(cog_make_continuation());
    cog_run_next(block, NULL, NULL);
    return NULL;
}
cog_modfunc fne_begin = {"Begin", COG_FUNC, fn_begin, "Call-with-current-continuation, on a block."};

// MARK: BUILTINS TABLES

static cog_modfunc* builtin_modfunc_table[] = {
    &fne_parser_rule_special_chars,
    &fne_parser_rule_break_chars,
    &fne_parser_handle_comments,
    &fne_parser_ignore_whitespace,
    &fne_parser_handle_def, // must be before fne_parser_handle_identifiers
    &fne_parser_handle_let,
    &fne_parser_handle_int,
    &fne_parser_handle_float,
    &fne_parser_handle_bool_true,
    &fne_parser_handle_bool_false,
    &fne_parser_handle_symbols, // must be before fne_parser_handle_identifiers
    &fne_parser_handle_open_paren,
    &fne_parser_handle_close_paren_or_eof,
    &fne_parser_handle_string,
    &fne_parser_handle_semicolon,
    &fne_parser_discard_informal_syntax,
    &fne_parser_handle_identifiers, // must be last i guess
    &fne_parser_parse_block_loop,
    &fne_parse,
    &fne_parser_nextitem,
    &fne_parser_handle_token,
    &fne_parser_transform_def_or_let,
    &fne_closure_restore_scope,
    &fne_closure_insert_new_scope,
    // math functions
    &fne_empty,
    &fne_plus,
    &fne_minus,
    &fne_times,
    &fne_divide,
    &fne_modulo,
    &fne_less,
    &fne_greater,
    &fne_lesseq,
    &fne_greatereq,
    &fne_pow,
    &fne_eq,
    &fne_random,
    &fne_sqrt,
    &fne_floor,
    &fne_round,
    &fne_ceil,
    &fne_abs,
    // trig functions
    &fne_sind,
    &fne_cosd,
    &fne_tand,
    &fne_sin,
    &fne_cos,
    &fne_tan,
    &fne_exp,
    &fne_ln,
    &fne_asind,
    &fne_acosd,
    &fne_atand,
    &fne_asin,
    &fne_acos,
    &fne_atan,
    &fne_sinhd,
    &fne_coshd,
    &fne_tanhd,
    &fne_sinh,
    &fne_cosh,
    &fne_tanh,
    // boolean functions
    &fne_or,
    &fne_and,
    &fne_not,
    &fne_xor,
    // control flow
    &fne_if,
    &fne_do,
    // type predicates
    &fne_is_number,
    &fne_is_symbol,
    &fne_is_integer,
    &fne_is_list,
    &fne_is_string,
    &fne_is_block,
    &fne_is_boolean,
    &fne_is_zero,
    &fne_is_io,
    // type assertions
    &fne_assert_number,
    &fne_assert_symbol,
    &fne_assert_string,
    &fne_assert_block,
    &fne_assert_boolean,
    &fne_assert_list,
    &fne_assert_io,
    // IO
    &fne_print,
    &fne_put,
    // list functions
    &fne_list,
    &fne_list_finish,
    &fne_first,
    &fne_rest,
    &fne_push,
    &fne_is_empty,
    // string functions
    &fne_append,
    &fne_substring,
    &fne_ordinal,
    &fne_character,
    &fne_split,
    &fne_lowercase,
    &fne_uppercase,
    // box functions
    &fne_box,
    &fne_unbox,
    &fne_set,
    // conversion functions
    &fne_number,
    &fne_show,
    // error handling
    &fne_error,
    // misc
    &fne_wait,
    &fne_stop,
    &fne_stack,
    &fne_clear,
    &fne_begin,
    NULL
};

static cog_object_method* builtin_objfunc_table[] = {
    &ome_int_show,
    &ome_int_exec,
    &ome_bool_show,
    &ome_bool_exec,
    &ome_float_show,
    &ome_float_exec,
    &ome_identifier_show,
    &ome_identifier_exec,
    &ome_symbol_show,
    &ome_symbol_exec,
    &ome_string_show,
    &ome_string_exec,
    &ome_iostring_write,
    &ome_iostring_getch,
    &ome_iostring_ungets,
    &ome_iostring_show,
    &ome_bfunction_exec,
    &ome_closure_exec,
    &ome_block_exec,
    &ome_block_show,
    &ome_def_or_let_exec,
    &ome_def_or_let_show,
    &ome_var_exec,
    &ome_int_hash,
    &ome_bool_hash,
    &ome_float_hash,
    &ome_identifier_hash,
    &ome_symbol_hash,
    &ome_string_hash,
    &ome_iostring_hash,
    &ome_bfunction_hash,
    &ome_closure_hash,
    &ome_block_hash,
    &ome_def_or_let_hash,
    &ome_var_hash,
    &ome_continuation_exec,
    NULL
};

static cog_obj_type* builtin_types[] = {
    &cog_ot_pointer,
    &cog_ot_owned_pointer,
    &cog_ot_list,
    &cog_ot_int,
    &cog_ot_bool,
    &cog_ot_float,
    &cog_ot_identifier,
    &cog_ot_symbol,
    &cog_ot_string,
    &ot_iostring,
    &ot_bfunction,
    &ot_parser_sentinel,
    &ot_eof,
    &ot_block,
    &ot_closure,
    &ot_def_or_let_special,
    &ot_var,
    &ot_box,
    &cog_ot_continuation,
    NULL
};

static cog_module builtins = {"BUILTINS", builtin_modfunc_table, builtin_objfunc_table, builtin_types};

static void install_builtins() {
    cog_add_module(&builtins);
}

// MARK: INITIALIZATION

static void cogni_debug_handler(int sig);
void cog_init() {
    signal(SIGSEGV, cogni_debug_handler);
    setlocale(LC_ALL, "");

    COG_GLOBALS.not_impl_sym = cog_make_identifier_c("[[Status::NotImplemented]]");
    COG_GLOBALS.error_sym = cog_make_identifier_c("[[Status::Error]]");
    COG_GLOBALS.on_enter_sym = cog_make_identifier_c("[[Status::OnEnterHandler]]");
    COG_GLOBALS.on_exit_sym = cog_make_identifier_c("[[Status::OnExitHandler]]");
    cog_push_new_scope(); // the global scope
    install_builtins();
}

// MARK: DEBUGGING

static void print_backtrace() {
    static void* bt[1024];
	int bt_sz = backtrace(bt, 1024);
    char** entries = backtrace_symbols(bt, bt_sz);
    for (int i = bt_sz - 1; i > 0; i--)
        // ignore entry #0 which is the current function
        printf("%s\n", entries[i]);
    fflush(stdout);
    free(entries);
}

static void cogni_debug_handler(int sig) {
	puts("Aaaaaa! I segfaulted!!\nTraceback (most recent call last):");
    debug_dump_stuff();
    abort();
}

static void debug_dump_stuff() {
    putchar('\n');
    print_backtrace();
    cog_printf("DEBUG: work stack: %O\nDEBUG: command queue: %O\n", COG_GLOBALS.stack, COG_GLOBALS.command_queue);
}

// MARK: PRINTF / FPRINTF / SPRINTF
// down here because long

void vprint_inner(cog_object* stream, const char* fmt, va_list args) {
    const char* p = fmt;
    while (*p) {
        // find index of first '%'
        const char* first_fmt = strchr(p, '%');
        if (first_fmt == NULL) {
            // no more format strings
            char* s;
            asprintf(&s, "%s", p);
            cog_fputs_imm(stream, s);
            free(s);
            break;
        }
        // print everything up to fmt
        char* s;
        int len = first_fmt - p;
        asprintf(&s, "%.*s", len, p);
        cog_fputs_imm(stream, s);
        free(s);
        p = first_fmt;
        if (*p == '%') {
            p++;
            char* format;
            int len = strcspn(p, "diuOoxXfFeEgGacAsp") + 1;
            asprintf(&format, "%%%.*s", len, p);

            char* buffer;
            switch (format[len]) {
                case 'O': {
                    cog_object* obj = va_arg(args, cog_object*);
                    cog_dump(obj, stream, strchr(format, '#') == NULL);
                    goto afterprint;
                }
                case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c': {
                    int val = va_arg(args, int);
                    asprintf(&buffer, format, val);
                    goto done;
                }
                case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                    double val = va_arg(args, double);
                    asprintf(&buffer, format, val);
                    goto done;
                }
                case 's': {
                    char* val = va_arg(args, char*);
                    asprintf(&buffer, format, val);
                    goto done;
                }
                case 'p': {
                    void* val = va_arg(args, void*);
                    asprintf(&buffer, format, val);
                    goto done;
                }
                case '%': {
                    asprintf(&buffer, "%%");
                    goto done;
                }
                default:
                    fprintf(stderr, "bad format %s\n", format);
                    abort();
            }
            done:
            cog_fputs_imm(stream, buffer);
            free(buffer);
            afterprint:
            free(format);
            p += len;
        }
    }
}

void cog_fprintf(cog_object* stream, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint_inner(stream, fmt, args);
    va_end(args);
}

void cog_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint_inner(COG_GLOBALS.stdout_stream, fmt, args);
    va_end(args);
}

cog_object* cog_sprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    cog_object* stream = cog_empty_io_string();
    vprint_inner(stream, fmt, args);
    va_end(args);
    return cog_iostring_get_contents(stream);
}
