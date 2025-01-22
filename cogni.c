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
} COG_GLOBALS = {0};

cog_object* cog_not_implemented() {
    return COG_GLOBALS.not_impl_sym;
}

cog_object* cog_error() {
    return COG_GLOBALS.error_sym;
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
    gc();
    assert(COG_GLOBALS.mem == NULL);
}

// MARK: MODULES

void free_pointer(cog_object* going) {
    free(going->as_ptr);
}

cog_obj_type cog_ot_pointer = {"Pointer", NULL};
cog_obj_type cog_ot_owned_pointer = {"OwnedPointer", NULL, free_pointer};

void cog_add_module(cog_module* module) {
    cog_object* modobj = cog_make_obj(&cog_ot_pointer);
    modobj->as_ptr = (void*)module;
    cog_push_to(&COG_GLOBALS.modules, modobj);
}

// MARK: UTILITY / LISTS

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

void cog_push_to(cog_object** stack, cog_object* item) {
    cog_object* cell = cog_make_obj(NULL);
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

cog_object* cog_dup_list_shallow(cog_object* str1) {
    cog_object* out = cog_emptystring();
    cog_object* tail = out;
    while (str1) {
        tail->data = str1->data;
        tail->next = cog_emptystring();
        tail = tail->next;
        str1 = str1->next;
    }
    tail->next = NULL;
    return out;
}

cog_object* cog_list_splice(cog_object** l1, cog_object* l2) {
    cog_object* tail = *l1;
    if (tail) {
        while (tail->next) tail = tail->next;
        tail->next = l2;
    }
    else {
        *l1 = l2;
    }
    return *l1;
}

void cog_reverse_list_inplace(cog_object** list) {
    cog_object* previous = NULL;
    while (*list) {
        cog_object* next = (*list)->next;
        (*list)->next = previous;
        previous = *list;
        *list = next;
    }
}

// MARK: ENVIRONMENT

void cog_set_var(cog_object* identifier, cog_object* value) {
    cog_object* top_scope = COG_GLOBALS.scopes->data;
    cog_object* pair = cog_assoc(top_scope, identifier, cog_same_identifiers);
    if (pair) {
        pair->next = value;
    } else {
        pair = cog_make_obj(NULL);
        pair->data = identifier;
        pair->next = value;
        cog_push_to(&top_scope->data, pair);
    }
}

cog_object* cog_get_var(cog_object* identifier, bool* found) {
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
    cog_push_scope(cog_make_obj(NULL));
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
            case COG_M_RUN_SELF: method_name = "COG_M_RUN_SELF"; break;
            case COG_M_STRINGIFY_SELF: method_name = "COG_M_STRINGIFY_SELF"; break;
            case COG_SM_PUTS: method_name = "COG_SM_PUTS"; break;
            case COG_SM_GETCH: method_name = "COG_SM_GETCH"; break;
            case COG_SM_UNGETS: method_name = "COG_SM_UNGETS"; break;
            default: method_name = "UNKNOWN_METHOD"; break;
        }
        fprintf(stderr, "error: %s not implemented for %s\n", method_name, obj->type ? obj->type->typename : "NULL");
        print_backtrace();
        abort();
    }
    return res;
}

cog_object* cog_mainloop(cog_object* status) {
    size_t next_gc = COG_GLOBALS.alloc_chunks * 2;
    while (COG_GLOBALS.command_queue) {
        // debug_dump_stuff();
        cog_object* cmd = cog_pop_from(&COG_GLOBALS.command_queue);
        if (cmd == NULL) break;
        cog_object* when = cmd->data;
        cog_object* which = cmd->next->data;
        cog_object* cookie = cmd->next->next;
        if (!status && when) continue;
        if (status && !cog_same_identifiers(status, when)) continue;

        cog_push(cookie);
        status = cog_run_well_known(which, COG_M_RUN_SELF);
        if (cog_same_identifiers(status, cog_not_implemented())) {
            cog_pop();
            cog_push(cog_sprintf("Can't run %O", which));
            status = cog_error();
        }
        // maybe do a GC
        if (COG_GLOBALS.alloc_chunks > next_gc) {
            // protect status in case it is nonstandard
            cog_walk(status, markobject, NULL);
            gc();
            next_gc = COG_GLOBALS.alloc_chunks * 2;
        }
    }
    trace();
    debug_dump_stuff();
    cog_printf("DEBUG: return status: %O\n", status);
    return status;
}

cog_object* cog_obj_push_self() {
    cog_pop();
    return NULL;
}

// MARK: NUMBERS

cog_obj_type ot_int = {"Integer", NULL};
cog_object* cog_box_int(cog_integer i) {
    cog_object* obj = cog_make_obj(&ot_int);
    obj->as_int = i;
    return obj;
}
cog_integer cog_unbox_int(cog_object* obj) {
    assert(obj->type == &ot_int);
    return obj->as_int;
}
cog_object* int_printself() {
    cog_object* num = cog_pop();
    cog_pop();
    char buffer[21];
    snprintf(buffer, sizeof(buffer), "%" PRId64, num->as_int);
    cog_push(cog_string(buffer));
    return NULL;
}
cog_object_method ome_int_stringify = {&ot_int, COG_M_STRINGIFY_SELF, int_printself};
cog_object_method ome_int_run = {&ot_int, COG_M_RUN_SELF, cog_obj_push_self};

cog_obj_type ot_bool = {"Boolean", NULL};
cog_object* cog_box_bool(bool i) {
    cog_object* obj = cog_make_obj(&ot_bool);
    obj->as_int = i;
    return obj;
}
bool cog_unbox_bool(cog_object* obj) {
    assert(obj->type == &ot_bool);
    return obj->as_int;
}
cog_object* bool_printself() {
    cog_object* num = cog_pop();
    cog_pop();
    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%s", num->as_int ? "True" : "False");
    cog_push(cog_string(buffer));
    return NULL;
}
cog_object_method ome_bool_stringify = {&ot_bool, COG_M_STRINGIFY_SELF, bool_printself};
cog_object_method ome_bool_run = {&ot_bool, COG_M_RUN_SELF, cog_obj_push_self};

cog_obj_type ot_float = {"Number", NULL};
cog_object* cog_box_float(cog_float i) {
    cog_object* obj = cog_make_obj(&ot_float);
    obj->as_float = i;
    return obj;
}
cog_float cog_unbox_float(cog_object* obj) {
    assert(obj->type == &ot_float);
    return obj->as_float;
}
cog_object* float_printself() {
    cog_object* num = cog_pop();
    cog_pop();
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%Lg", num->as_float);
    cog_push(cog_string(buffer));
    return NULL;
}
cog_object_method ome_float_stringify = {&ot_float, COG_M_STRINGIFY_SELF, float_printself};
cog_object_method ome_float_run = {&ot_float, COG_M_RUN_SELF, cog_obj_push_self};

// MARK: IDENTIFIERS

static cog_object* walk_identifier(cog_object* i, cog_walk_fun f, cog_object* arg) {
    (void)arg;
    if (i->as_int) return NULL;
    return i->next;
}
cog_obj_type ot_identifier = {"Identifier", walk_identifier};

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
        char* where = strchr(PACKEDALPHABET, tolower(*s));
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
        char* where = strchr(PACKEDALPHABET, tolower(cog_nthchar(string, i)));
        if (where == NULL) return false;
        res *= base;
        res += where - PACKEDALPHABET;
    }
    *out = (res << 1) | 1;
    return true;
}

cog_object* cog_explode_identifier(cog_object* i) {
    cog_object* buffer = cog_emptystring();
    cog_object* tail = buffer;
    if (i->as_packed_sym & 1) {
        // packed identifier
        cog_packed_identifier s = i->as_packed_sym >> 1;
        size_t base = strlen(PACKEDALPHABET);
        cog_packed_identifier div = 1;
        while ((div * base) < s)
            div *= base;
        int (*tr)(int) = toupper;
        for (;;) {
            cog_append_byte_to_buffer(&tail, tr(PACKEDALPHABET[(s / div) % base]));
            if (div == 1) break;
            div /= base;
            tr = tolower;
        }
    } else if (i->as_fun != NULL) {
        // builtin identifier
        for (const char* s = i->as_fun->name; *s; s++)
            cog_append_byte_to_buffer(&tail, *s);
    } else {
        // long identifier
        buffer = i->next;
    }
    return buffer;
}

cog_object* cog_make_identifier_c(const char* const name) {
    // TODO: intern?
    cog_object* out = cog_make_obj(&ot_identifier);
    // first try the builtin function names
    COG_ITER_LIST(COG_GLOBALS.modules, modobj) {
        cog_module* mod = (cog_module*)modobj->as_ptr;
        if (mod->table == NULL) continue;
        for (size_t i = 0; mod->table[i] != NULL; i++) {
            cog_modfunc* m = mod->table[i];
            // !!! not case sensitive
            if (m->when == COG_FUNC && !strcasecmp(m->name, name)) {
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
    cog_object* out = cog_make_obj(&ot_identifier);
    // first try the builtin function names
    COG_ITER_LIST(COG_GLOBALS.modules, modobj) {
        cog_module* mod = (cog_module*)modobj->as_ptr;
        if (mod->table == NULL) continue;
        for (size_t i = 0; mod->table[i] != NULL; i++) {
            cog_modfunc* m = mod->table[i];
            // !!! not case sensitive
            if (m->when == COG_FUNC && !cog_strcasecmp_c(string, m->name)) {
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

cog_object* m_stringify_identifier() {
    cog_object* i = cog_pop();
    cog_pop(); // ignore cookie
    cog_push(cog_explode_identifier(i));
    return NULL;
}
cog_object_method ome_identifier_stringify = {
    &ot_identifier,
    COG_M_STRINGIFY_SELF,
    m_stringify_identifier
};

cog_object* m_run_identifier() {
    cog_object* self = cog_pop();
    cog_object* cookie = cog_pop();
    // first look up definition
    bool found = false;
    cog_object* def = cog_get_var(self, &found);
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
cog_object_method ome_identifier_run = {&ot_identifier, COG_M_RUN_SELF, m_run_identifier};

bool cog_same_identifiers(cog_object* s1, cog_object* s2) {
    if (!s1 && !s2) return true;
    if (!s1 || !s2) return false;
    assert(s1->type == &ot_identifier);
    assert(s2->type == &ot_identifier);
    return !cog_strcasecmp(cog_explode_identifier(s1), cog_explode_identifier(s2));
}

// MARK: SYMBOLS

cog_obj_type ot_symbol = {"Symbol", cog_walk_only_next, NULL};

cog_object_method ome_symbol_run = {&ot_symbol, COG_M_RUN_SELF, cog_obj_push_self};

cog_object* cog_sym(cog_object* i) {
    cog_object* s = cog_make_obj(&ot_symbol);
    s->next = i;
    return s;
}

cog_object* m_symbol_stringify() {
    cog_object* sym = cog_pop();
    bool readably = cog_expect_type_fatal(cog_pop(), &ot_bool)->as_int;
    if (!readably) {
        cog_push(cog_explode_identifier(sym->next));
    } else {
        cog_push(cog_sprintf("\\%O", sym->next));
    }
    return NULL;
}
cog_object_method ome_symbol_stringify = {&ot_symbol, COG_M_STRINGIFY_SELF, m_symbol_stringify};

// MARK: BUFFERS

cog_obj_type ot_buffer = {"Buffer", cog_walk_only_next, NULL};

cog_object* cog_emptystring() {
    return cog_make_obj(&ot_buffer);
}

void cog_append_byte_to_buffer(cog_object** buffer, char data) {
    if ((*buffer)->stored_chars >= COG_MAX_CHARS_PER_BUFFER_CHUNK) {
        while ((*buffer)->next != NULL) {
            *buffer = (*buffer)->next;
        }
        cog_object* next = cog_emptystring();
        next->as_chars[0] = data;
        next->stored_chars = 1;
        (*buffer)->next = next;
        *buffer = next;
    }
    else {
        (*buffer)->as_chars[(*buffer)->stored_chars] = data;
        (*buffer)->stored_chars++;
    }
}

void cog_prepend_byte_to_buffer(cog_object** buffer, char data) {
    if ((*buffer) && (*buffer)->stored_chars < COG_MAX_CHARS_PER_BUFFER_CHUNK) {
        memmove((*buffer)->as_chars + 1, (*buffer)->as_chars, (*buffer)->stored_chars);
        (*buffer)->as_chars[0] = data;
        (*buffer)->stored_chars++;
    }
    else {
        cog_object* new_head = cog_emptystring();
        new_head->as_chars[0] = data;
        new_head->stored_chars = 1;
        new_head->next = *buffer;
        *buffer = new_head;
    }
}

size_t cog_strlen(cog_object* buffer) {
    size_t len = 0;
    while (buffer != NULL) {
        len += buffer->stored_chars;
        buffer = buffer->next;
    }
    return len;
}

char cog_nthchar(cog_object* buffer, size_t i) {
    while (i >= buffer->stored_chars) {
        if (buffer == NULL) return 0;
        i -= buffer->stored_chars;
        buffer = buffer->next;
    }
    if (!buffer || i >= buffer->stored_chars) return 0;
    return buffer->as_chars[i];
}

void b_set_nthchar(cog_object* buffer, size_t i, char c) {
    while (i >= buffer->stored_chars) {
        assert(buffer != NULL);
        i -= buffer->stored_chars;
        buffer = buffer->next;
    }
    assert(buffer && i < buffer->stored_chars);
    buffer->as_chars[i] = c;
}

void cog_insert_byte_to_buffer_at(cog_object** buffer, char data, size_t index) {
    cog_object* current = *buffer;

    while (current && index >= current->stored_chars) {
        index -= current->stored_chars;
        current = current->next;
    }

    if (!current) {
        // If index is out of bounds, append to the end
        cog_append_byte_to_buffer(buffer, data);
        return;
    }

    if (current->stored_chars < COG_MAX_CHARS_PER_BUFFER_CHUNK) {
        memmove(current->as_chars + index + 2, current->as_chars + index + 1, current->stored_chars - index);
        current->as_chars[index] = data;
        current->stored_chars++;
    } else {
        cog_prepend_byte_to_buffer(&current->next, current->as_chars[COG_MAX_CHARS_PER_BUFFER_CHUNK - 1]);
        memmove(current->as_chars + index + 2, current->as_chars + index + 1, current->stored_chars - index);
        current->stored_chars = COG_MAX_CHARS_PER_BUFFER_CHUNK;
        current->as_chars[index] = data;
    }
}

void cog_delete_byte_from_buffer_at(cog_object** buffer, size_t index) {
    cog_object** current = buffer;

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

cog_object* cog_string(const char* const cstr) {
    size_t n = strlen(cstr);
    cog_object* str = cog_emptystring();
    cog_object* tail = str;
    for (size_t i = 0; i < n; i++)
        cog_append_byte_to_buffer(&tail, cstr[i]);
    return str;
}

size_t cog_buffer_to_cstring(cog_object* buf, char* const str, size_t len) {
    char* p = str;
    while (buf && (p - str) < len) {
        size_t nmore = min(buf->stored_chars, len - (p - str));
        memcpy(p, buf->as_chars, nmore);
        p += nmore;
        buf = buf->next;
    }
    *p = 0;
    return p - str;
}

cog_object* cog_m_buffer_stringify() {
    cog_object* buffer = cog_pop();
    bool readably = cog_expect_type_fatal(cog_pop(), &ot_bool)->as_int;
    if (!readably) {
        cog_push(buffer);
    }
    else {
        cog_object* ebuf = cog_emptystring();
        cog_object* tail = ebuf;
        cog_append_byte_to_buffer(&tail, '"');
        cog_object* chunk = buffer;
        while (chunk) {
            for (size_t i = 0; i < chunk->stored_chars; i++) {
                bool special = false;
                char ch = cog_maybe_escape_char(chunk->as_chars[i], &special);
                if (special) cog_append_byte_to_buffer(&tail, '\\');
                cog_append_byte_to_buffer(&tail, ch);
            }
            chunk = chunk->next;
        }
        cog_append_byte_to_buffer(&tail, '"');
        cog_push(ebuf);
    }
    return NULL;
}
cog_object_method ome_buffer_stringify = {&ot_buffer, COG_M_STRINGIFY_SELF, cog_m_buffer_stringify};
cog_object_method ome_buffer_run = {&ot_buffer, COG_M_RUN_SELF, cog_obj_push_self};

cog_object* cog_make_character(char c) {
    // a character is just a one character buffer
    cog_object* obj = cog_emptystring();
    cog_append_byte_to_buffer(&obj, c);
    return obj;
}

cog_object* cog_strcat(cog_object* str1, cog_object* str2) {
    cog_object* str1clone = cog_dup_list_shallow(str1);
    return cog_list_splice(&str1clone, str2);
}

int cog_strcmp(cog_object* str1, cog_object* str2) {
    assert(str1->type == &ot_buffer);
    assert(str2->type == &ot_buffer);
    int i1 = 0, i2 = 0;
    while (str1 && str2) {
        char d = str1->as_chars[i1] - str2->as_chars[i2];
        if (d != 0) return d;
        i1++, i2++;
        if (i1 >= str1->stored_chars) {
            i1 = 0;
            str1 = str1->next;
        }
        if (i2 >= str2->stored_chars) {
            i2 = 0;
            str2 = str2->next;
        }
    }
    if (!str1 && !str2) return 0;
    if (str1) return 1;
    return -1;
}

int cog_strcasecmp(cog_object* str1, cog_object* str2) {
    assert(str1->type == &ot_buffer);
    assert(str2->type == &ot_buffer);
    int i1 = 0, i2 = 0;
    while (str1 && str2) {
        char d = tolower(str1->as_chars[i1]) - tolower(str2->as_chars[i2]);
        if (d != 0) return d;
        i1++, i2++;
        if (i1 >= str1->stored_chars) {
            i1 = 0;
            str1 = str1->next;
        }
        if (i2 >= str2->stored_chars) {
            i2 = 0;
            str2 = str2->next;
        }
    }
    if (!str1 && !str2) return 0;
    if (str1) return 1;
    return -1;
}

int cog_strcmp_c(cog_object* str1, const char* const str2) {
    assert(str1->type == &ot_buffer);
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
    assert(str1->type == &ot_buffer);
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

// MARK: FILES

static void df_close_file(cog_object* stream) {
    if (stream->as_ptr) {
        FILE* f = stream->as_ptr;
        if (f != stdin && f != stdout && f != stderr) {
            fclose(f);
            stream->as_ptr = NULL;
        }
    }
}
cog_obj_type ot_file = {"File", cog_walk_only_next, df_close_file};

cog_object* cog_make_filestream(FILE* f, cog_object* filename) {
    assert(filename == NULL || filename->type == &ot_buffer);
    cog_object* fo = cog_make_obj(&ot_file);
    fo->as_ptr = (void*)f;
    fo->next = filename;
    return fo;
}

cog_object* cog_open_file(const char* const filename, const char* const mode) {
    return cog_make_filestream(fopen(filename, mode), cog_string(filename));
}

static cog_object* m_file_write() {
    cog_object* file = cog_pop();
    cog_object* buf = cog_expect_type_fatal(cog_pop(), &ot_buffer);
    FILE* f = file->as_ptr;
    while (buf) {
        fprintf(f, "%*s", buf->stored_chars, buf->as_chars);
        fflush(f);
        buf = buf->next;
    }
    return NULL;
}
cog_object_method ome_file_write = {&ot_file, COG_SM_PUTS, m_file_write};

static cog_object* m_file_getch() {
    cog_object* file = cog_pop();
    FILE* f = file->as_ptr;
    if (feof(f)) cog_push(cog_eof());
    else cog_push(cog_make_character(fgetc(f)));
    return NULL;
}
cog_object_method ome_file_getch = {&ot_file, COG_SM_GETCH, m_file_getch};

static cog_object* m_file_ungets() {
    cog_object* file = cog_pop();
    cog_object* buf = cog_expect_type_fatal(cog_pop(), &ot_buffer);
    FILE* f = file->as_ptr;
    while (buf) {
        for (int i = 0; i < buf->stored_chars; i++)
            ungetc(buf->as_chars[i], f);
        buf = buf->next;
    }
    return NULL;
}
cog_object_method ome_file_ungets = {&ot_file, COG_SM_UNGETS, m_file_ungets};

cog_object* m_file_stringify() {
    cog_object* file = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<File %O at pos %li>", file->next, ftell((FILE*)file->as_ptr)));
    return NULL;
}
cog_object_method ome_file_stringify = {&ot_file, COG_M_STRINGIFY_SELF, m_file_stringify};

// MARK: STRING STREAMS

cog_obj_type ot_iostring = {"IOString", cog_walk_both, NULL};
cog_object* cog_empty_io_string() {
    cog_object* stream = cog_make_obj(&ot_iostring);
    stream->data = cog_box_int(0); // the cursor position
    stream->next = cog_make_obj(NULL);
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
    cog_object* buf = cog_expect_type_fatal(cog_pop(), &ot_buffer);
    if (cog_strlen(stream->next->data) > 0) {
        cog_push(cog_string("can't write until ungets stack is empty"));
        return cog_error();
    }
    cog_integer pos = stream->data->as_int;
    cog_object* data = cog_iostring_get_contents(stream);
    cog_object* tail = data;
    size_t len = cog_strlen(data);
    while (buf) {
        for (int i = 0; i < buf->stored_chars; i++) {
            if (pos < len) b_set_nthchar(data, pos, buf->as_chars[i]);
            else cog_append_byte_to_buffer(&tail, buf->as_chars[i]);
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
        cog_delete_byte_from_buffer_at(&stream->next->data, 0);
        cog_push(cog_make_character(c));
        return NULL;
    }
    cog_integer pos = stream->data->as_int;
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
    cog_object* buf = cog_expect_type_fatal(cog_pop(), &ot_buffer);
    size_t len = cog_strlen(buf);
    for (size_t iplus1 = len; iplus1 > 0; iplus1--) {
        cog_prepend_byte_to_buffer(&stream->next->data, cog_nthchar(buf, iplus1 - 1));
    }
    return NULL;
}
cog_object_method ome_iostring_ungets = {&ot_iostring, COG_SM_UNGETS, m_iostring_ungets};

cog_object* m_iostring_stringify() {
    cog_object* stream = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<IOstring at pos %O of %O>", stream->data, cog_iostring_get_contents(stream)));
    return NULL;
}
cog_object_method ome_iostring_stringify = {&ot_iostring, COG_M_STRINGIFY_SELF, m_iostring_stringify};

// MARK: BUILTIN FUNCTION OBJECTS

cog_obj_type ot_bfunction = {"BuiltinFunction", NULL};

cog_object* cog_make_bfunction(cog_modfunc* func) {
    assert(func);
    assert(func->when == COG_FUNC);
    cog_object* obj = cog_make_obj(&ot_bfunction);
    obj->as_fun = func;
    return obj;
}

cog_object* m_bfunction_run() {
    cog_object* self = cog_pop();
    cog_modfunc* f = self->as_fun;
    // jump straight into the function
    return f->fun();
}
cog_object_method ome_bfunction_run = {&ot_bfunction, COG_M_RUN_SELF, m_bfunction_run};

// MARK: BLOCKS

cog_obj_type ot_block = {"Block", cog_walk_both, NULL};

cog_object* cog_make_block(cog_object* commands) {
    assert(!commands || !commands->type);
    cog_object* obj = cog_make_obj(&ot_block);
    obj->data = NULL;
    obj->next = commands;
    return obj;
}

cog_object* m_block_run() {
    cog_object* self = cog_pop();
    // TODO: this is wrong. Block obj when run should push another obj that when run should do this.
    // TODO: closure'd scopes.
    // TODO: push scope teardown command.
    cog_object* head_existing = COG_GLOBALS.command_queue;
    COG_GLOBALS.command_queue = NULL;
    COG_ITER_LIST(self->next, cmd) cog_run_next(cmd, NULL, NULL);
    cog_reverse_list_inplace(&COG_GLOBALS.command_queue);
    cog_list_splice(&COG_GLOBALS.command_queue, head_existing);
    // TODO: push new scope command.
    return NULL;
}
cog_object_method ome_block_run = {&ot_block, COG_M_RUN_SELF, m_block_run};

cog_object* m_block_stringify() {
    cog_object* block = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<Block %O>", block->next));
    return NULL;
}
cog_object_method ome_block_stringify = {&ot_block, COG_M_STRINGIFY_SELF, m_block_stringify};

// MARK: DUMPER

static bool make_refs_list(cog_object* obj, cog_object* alist_header) {
    cog_object* entry = cog_assoc(alist_header->data, obj, cog_same_pointer);
    if (entry) {
        entry->next = cog_box_int(2);
        return false;
    }
    cog_object* pair = cog_make_obj(NULL);
    pair->data = obj;
    pair->next = cog_box_int(1);
    cog_push_to(&alist_header->data, pair);
    // plain cons cells are the only "interesting" thing
    return obj->type == NULL;
}

static cog_integer reffed(cog_object* obj, cog_object* alist, cog_integer* counter) {
    cog_object* entry = cog_assoc(alist, obj, cog_same_pointer);
    if (entry) {
        cog_integer value = entry->next->as_int;
        if (value < 0) {
            return value;
        }
        if (value > 1) {
            cog_integer my_id = (*counter)++;
            entry->next = cog_box_int(-my_id);
            return my_id;
        }
    }
    return 0;
}

static void pr_refs_recursive(cog_object* obj, cog_object* alist, cog_object* stream, cog_integer* counter, bool readably) {
    char buffer[256];
    if (obj == NULL) {
        cog_fputs_imm(stream, "()");
        return;
    }
    // test if it's in the table
    cog_integer ref = reffed(obj, alist, counter);
    if (ref < 0) {
        snprintf(buffer, sizeof(buffer), "#%" PRId64 "#", -ref);
        cog_fputs_imm(stream, buffer);
        return;
    }
    if (ref) {
        snprintf(buffer, sizeof(buffer), "#%" PRId64 "=", ref);
        cog_fputs_imm(stream, buffer);
    }
    if (obj->type) {
        cog_push(cog_box_bool(readably)); // readably = true
        if (cog_same_identifiers(cog_run_well_known(obj, COG_M_STRINGIFY_SELF), cog_not_implemented())) {
            cog_pop();
            snprintf(buffer, sizeof(buffer), "#<%s: %p %p>", obj->type->typename, obj->data, obj->next);
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
            cog_integer ref = reffed(obj, alist, counter);
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
    cog_object* alist_header = cog_make_obj(NULL);
    cog_integer counter = 1;
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
    if (!modlist) goto error;
    // handle what's in the buffer
    cog_module* curr_mod = modlist->data->as_ptr;
    if (!curr_mod->table) goto nextmod;
    cog_modfunc* curr_func = curr_mod->table[index->as_int];
    if (!curr_func) goto nextmod;
    if (curr_func->when != COG_PARSE_TOKEN_HANDLER) goto nextfun;
    if (curr_func->name && buffer->type == &ot_buffer && cog_strcasecmp_c(buffer, curr_func->name))
        goto nextfun;
    if (curr_func->name && buffer->type != &ot_buffer) goto nextfun;

    cog_object* cookie2 = cog_make_obj(NULL);
    cookie2->data = buffer;
    cookie2->next = stream;
    cog_push(cookie2);
    cog_object* res = curr_func->fun();
    if (cog_same_identifiers(res, cog_not_implemented())) {
        cog_pop();
        if (buffer->type == &ot_buffer && cog_strlen(buffer) == 0) {
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
    cog_run_well_known_strict(buffer, COG_M_STRINGIFY_SELF);
    COG_RETURN_ERROR(cog_strcat(cog_string("PARSE ERROR: could not handle token "), cog_pop()));

    retry:
    cog_run_next(cog_make_identifier_c("[[Parser::HandleToken]]"), NULL, cookie);
    return NULL;
}
cog_modfunc fne_parser_handle_token = {
    "[[Parser::HandleToken]]",
    COG_FUNC,
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

    if (curr_char->type != &ot_eof && cog_strlen(curr_char) != 1) goto firstchar;
    int ch = curr_char->type != &ot_eof ? cog_nthchar(curr_char, 0) : EOF;

    if (!modlist) goto nextchar;
    // test current character
    cog_module* curr_mod = modlist->data->as_ptr;
    if (!curr_mod->table) goto nextmod;
    cog_modfunc* curr_func = curr_mod->table[index->as_int];
    if (!curr_func) goto nextmod;
    if (curr_func->when != COG_PARSE_INDIV_CHAR && curr_func->when != COG_PARSE_END_CHAR) goto nextfun;
    if (curr_func->when == COG_PARSE_END_CHAR && cog_strlen(buffer) == 0) goto nextfun;
    if (curr_func->name != NULL && strchr(curr_func->name, ch) == NULL) goto nextfun;

    cog_object* cookie2 = stream;
    cog_push_to(&cookie2, curr_char);
    cog_push_to(&cookie2, buffer);
    cog_object* old_top = COG_GLOBALS.stack->data;
    cog_push(cookie2);
    cog_object* res = curr_func->fun();
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
    if (ch != EOF) cog_append_byte_to_buffer(&tail, ch);

    firstchar:
    COG_RUN_WKM_RETURN_IF_ERROR(stream, COG_SM_GETCH);
    cookie->next->next->next->next = cog_pop();
    cookie->next->data = COG_GLOBALS.modules;

    loop:
    cog_run_next(cog_make_identifier_c("[[Parser::NextItem]]"), NULL, cookie);
    return NULL;

    end_of_token:
    if (cog_strlen(buffer) == 0 && curr_char->type == &ot_buffer) buffer = curr_char;
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
    COG_FUNC,
    fn_parser_nextitem,
    doc_parser_internals,
};

cog_object* fn_parser_rule_special_chars() {
    cog_object* cookie = cog_pop();
    cog_object* ch = cookie->next->data;
    if (ch->type == &ot_buffer) {
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
    if (cookie->next->data->type == &ot_buffer) {
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
    if (s->type == &ot_buffer) {
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
    if (s->type == &ot_buffer) {
        cog_buffer_to_cstring(s, buffer, sizeof(buffer));
        cog_integer i;
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
    if (s->type == &ot_buffer) {
        cog_buffer_to_cstring(s, buffer, sizeof(buffer));
        cog_float i;
        int len = 0;
        int filled = sscanf(buffer, "%Lg%n", &i, &len);
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
    if (s->type == &ot_buffer) {
        char first = cog_nthchar(s, 0);
        if (first == '\\') {
            cog_delete_byte_from_buffer_at(&s, 0);
            if (!all_valid_for_ident(s)) {
                cog_prepend_byte_to_buffer(&s, '\\');
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
    if (s->type == &ot_buffer) {
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
    if (s->type == &ot_buffer) {
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
                    cog_append_byte_to_buffer(&tail, '\\');
                } else {
                    ch = ch2;
                }
            }
        }
        cog_append_byte_to_buffer(&tail, ch);
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
cog_obj_type ot_pusher_special = {"[[Parser::Special::LetPusher]]", cog_walk_only_next, NULL};

cog_object* make_def_or_let_special_obj(cog_object* what, bool is_def) {
    cog_object* obj = cog_make_obj(&ot_def_or_let_special);
    obj->as_int = is_def;
    obj->next = what;
    return obj;
}

cog_object* make_pusher_special_obj(cog_object* what) {
    cog_object* obj = cog_make_obj(&ot_pusher_special);
    obj->next = what;
    return obj;
}

cog_object* m_def_or_let_run() {
    cog_object* self = cog_pop();
    bool is_def = self->as_int;
    cog_object* symbol = self->next;
    cog_object* value = cog_pop();
    cog_set_var(symbol, is_def ? value : make_pusher_special_obj(value));
    return NULL;
}
cog_object_method ome_def_or_let_run = {&ot_def_or_let_special, COG_M_RUN_SELF, m_def_or_let_run};

cog_object* m_def_or_let_stringify() {
    cog_object* self = cog_pop();
    bool is_def = self->as_int;
    cog_object* symbol = self->next;
    cog_push(cog_sprintf("<%s %O>", is_def ? "Def" : "Let", symbol));
    return NULL;
}
cog_object_method ome_def_or_let_stringify = {&ot_def_or_let_special, COG_M_STRINGIFY_SELF, m_def_or_let_stringify};

cog_object* m_pusher_run_self() {
    cog_object* self = cog_pop();
    cog_push(self->next);
    return NULL;
}
cog_object_method ome_pusher_run = {&ot_pusher_special, COG_M_RUN_SELF, m_pusher_run_self};

cog_object* fn_parser_transform_def_or_let() {
    cog_object* cookie = cog_pop();
    bool is_def = cookie->as_int;
    cog_object* what = cog_pop();
    if (!what || what->type != &ot_identifier) goto error;
    cog_push(make_def_or_let_special_obj(what, is_def));
    return NULL;

    error:
    cog_push(cog_sprintf("expected identifier after %s not %O", is_def ? "Def" : "Let", what));
    return cog_error();
}
cog_modfunc fne_parser_transform_def_or_let = {
    "[[Parser::TransformDefOrLet]]",
    COG_FUNC,
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
    if (s->type == &ot_buffer) {
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
    if (ijp->next->type == &ot_buffer && stopwhen->type == &ot_buffer && !cog_strcmp(ijp->next, stopwhen)) goto stop;
    // else it is an error
    cog_push(ijp->next->type == &ot_buffer ? cog_sprintf("unexpected %O", ijp->next): cog_string("unexpected EOF"));
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
    COG_FUNC,
    fn_parser_parse_block_loop,
    doc_parser_internals
};

cog_object* fn_parse() {
    cog_pop(); // discard cookie
    cog_object* stream = cog_pop();
    if (stream == NULL) {
        cog_push(NULL);
        return NULL;
    }
    if (stream->type == &ot_buffer) {
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

// TODO: implement ALL of these

// MARK: BUILTINS TABLES

static cog_modfunc* builtin_modfunc_table[] = {
    &fne_parse,
    &fne_parser_nextitem,
    &fne_parser_rule_special_chars,
    &fne_parser_rule_break_chars,
    &fne_parser_handle_comments,
    &fne_parser_ignore_whitespace,
    &fne_parser_handle_token,
    &fne_parser_handle_def, // must be before fne_parser_handle_identifiers
    &fne_parser_handle_let,
    &fne_parser_transform_def_or_let,
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
    NULL
};

static cog_object_method* builtin_objfunc_table[] = {
    &ome_int_stringify,
    &ome_int_run,
    &ome_bool_stringify,
    &ome_bool_run,
    &ome_float_stringify,
    &ome_float_run,
    &ome_identifier_stringify,
    &ome_identifier_run,
    &ome_symbol_stringify,
    &ome_symbol_run,
    &ome_file_write,
    &ome_file_getch,
    &ome_file_ungets,
    &ome_file_stringify,
    &ome_buffer_stringify,
    &ome_buffer_run,
    &ome_iostring_write,
    &ome_iostring_getch,
    &ome_iostring_ungets,
    &ome_iostring_stringify,
    &ome_bfunction_run,
    &ome_block_run,
    &ome_block_stringify,
    &ome_def_or_let_run,
    &ome_def_or_let_stringify,
    &ome_pusher_run,
    NULL
};

static cog_obj_type* builtin_types[] = {
    &cog_ot_pointer,
    &cog_ot_owned_pointer,
    &ot_int,
    &ot_bool,
    &ot_float,
    &ot_identifier,
    &ot_symbol,
    &ot_buffer,
    &ot_file,
    &ot_iostring,
    &ot_bfunction,
    &ot_parser_sentinel,
    &ot_eof,
    &ot_block,
    &ot_def_or_let_special,
    NULL
};

static cog_module builtins = {"BUILTINS", builtin_modfunc_table, builtin_objfunc_table, builtin_types};

static void install_builtins() {
    cog_add_module(&builtins);
}

static void cogni_debug_handler(int sig);
void cog_init() {
    signal(SIGSEGV, cogni_debug_handler);

    COG_GLOBALS.not_impl_sym = cog_make_identifier_c("[[Status::NotImplemented]]");
    COG_GLOBALS.error_sym = cog_make_identifier_c("[[Status::Error]]");
    cog_set_stdout(cog_open_file("/dev/stdout", "w"));
    cog_set_stdin(cog_open_file("/dev/stdin", "r"));
    cog_set_stderr(cog_open_file("/dev/stderr", "w"));
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
        char* first_fmt = strchr(p, '%');
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
