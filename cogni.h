#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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

#define COG_MAX_CHARS_PER_BUFFER_CHUNK (__SIZEOF_POINTER__ - 2)

// MARK: TYPES N' STUFF

typedef struct cog_object cog_object;
typedef const struct cog_module cog_module;
typedef const struct cog_obj_type cog_obj_type;
typedef const struct cog_modfunc cog_modfunc;
typedef const struct cog_object_method cog_object_method;

typedef uint64_t cog_packed_identifier;

typedef cog_object* (*cog_function)();

struct cog_object {
    cog_obj_type* type;
    bool marked;
    union {
        cog_object* data;
        int64_t as_int;
        double as_float;
        void* as_ptr;
        cog_modfunc* as_fun;
        cog_packed_identifier as_packed_sym;
        char as_chars[COG_MAX_CHARS_PER_BUFFER_CHUNK + 1];
        #define stored_chars as_chars[COG_MAX_CHARS_PER_BUFFER_CHUNK]
    };
    cog_object* next;
};
static_assert(offsetof(cog_object, as_chars) + COG_MAX_CHARS_PER_BUFFER_CHUNK + 1 <= offsetof(cog_object, next), "bad object");

typedef bool (*cog_walk_fun)(cog_object* walking, cog_object* cookie);
typedef bool (*cog_chk_eq_fun)(cog_object* a, cog_object* b);

struct cog_obj_type {
    const char* const typename;
    cog_object* (*walk)(cog_object* obj, cog_walk_fun recurse, cog_object* arg);
    void (*destroy)(cog_object* obj);
};

enum cog_api_func_phase {
    COG_FUNC, // (...args -- ...any)
    COG_COOKIEFUNC, // (...args cookie -- ...any)
    COG_PARSE_TOKEN_HANDLER, // (stream buffer -- ...any)
    COG_PARSE_INDIV_CHAR, // (stream ch -- ...any)
    COG_PARSE_END_CHAR, // (stream ch -- ...any)
};
struct cog_modfunc {
    const char* const name;
    enum cog_api_func_phase when;
    cog_function fun;
    const char* const doc;
};

enum cog_well_known_method {
    // for all objects
    COG_M_EXEC, // (cookie self -- ...blah)
    COG_M_HASH, // (self -- hash)
    COG_M_SHOW, // (readably self -- string)
    COG_M_SHOW_REC, // (ctx readably self -- string)
    COG_M_SERIALIZE, // (self -- buffer)
    COG_M_UNSERIALIZE, // (buffer trash -- obj)
    // COG_M_COMPARE, // (other self -- result)

    // for streams
    COG_SM_GETCH, // (stream -- buffer)
    COG_SM_PUTS, // (buffer stream -- )
    COG_SM_UNGETS, // (buffer stream -- )

    // for containers
    // COG_CM_STORE, // (value indexer self -- )
    // COG_CM_INDEX, // (indexer self -- value)
    // COG_CM_GET_LENGTH, // (self -- length)
    // COG_CM_SPLICE, // (inserted ndelete self -- deleted)
};
struct cog_object_method {
    cog_obj_type* type_for;
    enum cog_well_known_method wkm;
    cog_function func;
};

struct cog_module {
    const char* const modname;
    cog_modfunc** table;
    cog_object_method** mtab;
    cog_obj_type** types;
};

/**
 * Adds a module to the system.
 * @param module
 */
void cog_add_module(cog_module*);

/**
 * Returns the number of `cog_object`s currently in use.
 */
size_t cog_get_num_cells_used();

/**
 * Returns the fragmentation level of Cognate's garbage collector. This value
 * is calculated as: `cells allocated ÷ cells used`. Lower values (closer to 1.0)
 * are better.
 */
double cog_get_gc_fragmentation();

/**
 * Makes an object immortal, preventing it from being garbage collected
 * until `cog_quit()` is called.
 * @param obj
 */
void cog_make_immortal(cog_object*);

cog_object* cog_get_stdout();
cog_object* cog_get_stderr();
cog_object* cog_get_stdin();
void cog_set_stdout(cog_object*);
void cog_set_stderr(cog_object*);
void cog_set_stdin(cog_object*);

/**
 * Creates a new object of the specified type.
 * @param type The type of the object to create.
 * @return The newly created object.
 */
cog_object* cog_make_obj(cog_obj_type* type);

/**
 * Walks through the object graph starting from the root object.
 * @param root The root object to start walking from.
 * @param callback The callback function to call for each object.
 * @param cookie Additional data to pass to the callback function.
 */
void cog_walk(cog_object*, cog_walk_fun, cog_object*);

/**
 * A `cog_walk_fun` implementation for objects that store non-`cog_object*`s in
 * `data` and `cog_object*`s in `next`.
 */
cog_object* cog_walk_only_next(cog_object*, cog_walk_fun, cog_object*);

/**
 * A `cog_walk_fun` implementation for objects that store `cog_object*`s in both
 * `data` and `next`.
 */
cog_object* cog_walk_both(cog_object*, cog_walk_fun, cog_object*);

/**
 * Boxes an integer into a cog_object.
 */
cog_object* cog_box_int(int64_t);

/**
 * Unboxes an integer from a `cog_object`, or calls `abort()` if it
 * is not an integer object.
 */
int64_t cog_unbox_int(cog_object*);

/**
 * Boxes a float into a `cog_object`.
 */
cog_object* cog_box_float(double);

/**
 * Unboxes a float from a `cog_object`, or calls `abort()` if it
 * is not a float object.
 */
double cog_unbox_float(cog_object*);

/**
 * Boxes a boolean into a `cog_object`.
 */
cog_object* cog_box_bool(bool);

/**
 * Unboxes a boolean from a `cog_object`, or calls `abort()` if it
 * is not a boolean object.
 */
bool cog_unbox_bool(cog_object*);

/**
 * Creates a identifier from a C string. The identifier may be a packed identifier, a long
 * identifier, or a builtin identifier as are available.
 */
cog_object* cog_make_identifier_c(const char* const s);

/**
 * Same as `cog_make_identifier_c` except uses an existing Cognate string.
 */
cog_object* cog_make_identifier(cog_object*);

/**
 * Explodes an identifier into a buffer.
 * @param i The identifier to explode.
 * @return The buffer containing the exploded identifier.
 */
cog_object* cog_explode_identifier(cog_object*, bool);

/**
 * Checks if two identifiers are the same.
 * @return `true` if the identifiers are the same (both `NULL` or
 * both the same characters internally), `false` otherwise.
 */
bool cog_same_identifiers(cog_object*, cog_object*);

cog_object* cog_sym(cog_object*);

/**
 * Compares two strings.
 * @return A negative number if str1 comes before str2, zero if they are equal, or
 * a positive number if str2 comes before str1.
 */
int cog_strcmp(cog_object*, cog_object*);
int cog_strncmp(cog_object*, cog_object*, size_t);

/**
 * Same as `cog_strcmp` except ignores case.
 */
int cog_strcasecmp(cog_object*, cog_object*);

/**
 * Compares a Cognate string to a C string, but without converting the C string to
 * a Cognate string first.
 * @return A negative number if str1 comes before str2, zero if they are equal, or
 * a positive number if str2 comes before str1.
 */
int cog_strcmp_c(cog_object*, const char* const str2);

/**
 * Same as `cog_strcmp_c` except ignores case.
 */
int cog_strcasecmp_c(cog_object*, const char* const str2);

/**
 * Concatenates two strings. The input strings are not modified, but the returned string
 * will share the same chunks as `str2`.
 */
cog_object* cog_strappend(cog_object*, cog_object*);

/**
 * Creates a string from a C string.
 */
cog_object* cog_string(const char* const s);

/**
 * Puts the string into the given C buffer, and returns the number of characters stored.
 * @param len The size of the buffer. If the string is too long to fit into the given buffer,
 * the value returned will be equal to len.
 */
size_t cog_string_to_cstring(cog_object*, char* const cstr, size_t);

/**
 * Creates a string from a C char array with a known length.
 */
cog_object* cog_string_from_bytes(const char* const cstr, size_t);

/**
 * Creates an empty string.
 */
cog_object* cog_emptystring();

/**
 * Return the length of the string (the number of bytes stored, which may
 * be greater than the length to the first null byte, since these are allowed).
 */
size_t cog_strlen(cog_object*);

/**
 * Return the Nth character in the string, starting from 0. If the index is out of
 * bounds 0 is returned, but that may also be a valid character in the string.
 * Check against `cog_strlen` to make sure it is really out of bounds.
 */
char cog_nthchar(cog_object*, size_t);

/**
 * Appends a byte to a buffer.
 * @param buffer A pointer to the buffer to append the byte to.
 * @param data The byte to append. Null bytes are not treated specially
 * and can be put in the middle of a buffer.
 */
void cog_string_append_byte(cog_object**, char);

/**
 * Prepends a byte to a buffer.
 * @param buffer A pointer to the buffer to append the byte to.
 */
void cog_string_prepend_byte(cog_object**, char);

/**
 * Inserts a byte into a buffer at a specific index.
 * @param buffer A pointer to the buffer to append the byte to.
 */
void cog_string_insert_char(cog_object**, char, size_t);

/**
 * Deletes a byte from a buffer at a specific index.
 * @param buffer A pointer to the buffer to append the byte to.
 */
void cog_string_delete_char(cog_object**, size_t);

/**
 * Return a new (not shared) substring of the string str.
 */
cog_object* cog_substring(cog_object*, size_t, size_t);

/**
 * Creates an empty IO string stream.
 */
cog_object* cog_empty_io_string();

/**
 * Wrap a string in an IO String writer.
 */
cog_object* cog_iostring_wrap(cog_object*);

cog_object* cog_iostring_get_contents(cog_object*);

/**
 * Writes a literal character to a stream.
 */
void cog_fputchar_imm(cog_object*, char);

/**
 * Writes a literal C string to a stream.
 */
void cog_fputs_imm(cog_object*, const char* const s);

/**
 * Wraps a `cog_modfunc*` into an object that can be run.
 * The modfunc mush have a `when` of `COG_FUNC`.
 */
cog_object* cog_make_bfunction(cog_modfunc*);

/**
 * Expects an object to be of a specific type, aborting if it is not.
 * @return The object if it is of the expected type.
 */
cog_object* cog_expect_type_fatal(cog_object*, cog_obj_type*);

/**
 * Creates a new block object from a list of commands.
 * @param commands The list of commands to create the block from.
 * @return The newly created block object.
 */
cog_object* cog_make_block(cog_object*);

/**
 * Creates a new closure object from a block and a list of scopes.
 */
cog_object* cog_make_closure(cog_object*, cog_object*);

/**
 * Creates a new character object from a character.
 * @param c The character to create the object from.
 * @return The newly created character object.
 */
cog_object* cog_make_character(char c);

/**
 * Creates a new EOF object.
 * @return The newly created EOF object.
 */
cog_object* cog_eof();

/**
 * Unescapes a character.
 */
char cog_unescape_char(char);

/**
 * Escapes a character if necessary.
 * @param did_escape A pointer to a boolean that will be set
 * to true if the character was escaped.
 */
char cog_maybe_escape_char(char, bool*);

/**
 * Destructively splices two lists or strings together.
 * @param l1 Pointer to the first list or string, which will be modified to point to the second list or string.
 * @return The spliced list or string. Usually the same as `*l1` but not always.
 */
cog_object* cog_list_splice(cog_object**, cog_object*);
#define cog_strcat cog_list_splice

/**
 * Duplicates a list shallowly. The items in the list are not copied.
 * This also works for duplicating a string.
 */
cog_object* cog_clone_list_shallow(cog_object*);
#define cog_strdup cog_clone_list_shallow

void cog_reverse_list_inplace(cog_object**);

/**
 * Pushes an object onto a stack.
 * @param stack A pointer to the stack to push the object onto,
 * which will be set to the new head.
 */
void cog_push_to(cog_object**, cog_object*);

/**
 * Pops an object from a stack.
 * @param stack A pointer to the stack to pop the object from,
 * which will be set to the new head.
 * @return The popped object.
 */
cog_object* cog_pop_from(cog_object** stack);

/**
 * Dumps an object to a stream.
 */
void cog_dump(cog_object*, cog_object*, bool);

/**
 * printf() to Cognate's internal stdout, but with %O you can dump an object.
 * There are some slight restrictions. For best behavior only use one-argument format
 * things (no `%.*s` variable precision things)
 */
void cog_printf(const char* fmt, ...);

/**
 * Like cog_printf except to a different stream.
 */
void cog_fprintf(cog_object* stream, const char* fmt, ...);

/**
 * Like cog_printf() but returns a Cognate string instead of printing to stdout.
 */
cog_object* cog_sprintf(const char* fmt, ...);

/**
 * Initializes the system.
 */
void cog_init();

/**
 * Frees all `cog_object`s (even ones made immortal) and resets system state.
 * Calls `abort()` if cleanup fails.
 */
void cog_quit();

/**
 * Pushes an object onto the global work stack.
 */
void cog_push(cog_object*);

/**
 * Pops an object from the global work stack.
 */
cog_object* cog_pop();

/**
 * Returns true if the global work stack is empty.
 */
bool cog_is_stack_empty();

/**
 * Returns the length of the global work stack.
 */
size_t cog_stack_length();

/**
 * Returns true if the global work stack has at least `n` items.
 */
bool cog_stack_has_at_least(size_t n);

/**
 * Queues the item to be run next in the main loop.
 * @param item The item to run.
 * @param when The status to run the item with.
 * @param cookie Additional data to pass to the item.
 */
void cog_run_next(cog_object*, cog_object*, cog_object*);

/**
 * Runs a well-known method on an object.
 * @param obj The object to run the method on.
 * @param meth The method to run.
 * @return The status returned by running the method.
 * Can be `cog_not_implemented()` if the method isn't defined on this type of object.
 */
cog_object* cog_run_well_known(cog_object*, enum cog_well_known_method);

/**
 * Runs a well-known method on an object, aborting if the method is not implemented.
 * @param obj The object to run the method on.
 * @param meth The method to run.
 * @return The status returned by running the method.
 */
cog_object* cog_run_well_known_strict(cog_object*, enum cog_well_known_method);

/**
 * Runs the main loop of the system until the command queue is empty.
 * This may invoke the garbage collector.
 * @param status The initial status.
 * @return The final status.
 */
cog_object* cog_mainloop(cog_object*);

/**
 * Returns the not implemented status identifier.
 */
cog_object* cog_not_implemented();

/**
 * Returns the error status identifier.
 */
cog_object* cog_error();

/**
 * Returns the status identifier used for context teardown handlers.
 */
cog_object* cog_on_exit();

/**
 * Returns the status identifier used for context setup handlers.
 */
cog_object* cog_on_enter();

/**
 * An implementation for the `COG_M_RUN_SELF` well-known method for
 * objects that evaluate to themselves.
 */
cog_object* cog_obj_push_self();

/**
 * Sets a variable in the current scope.
 */
void cog_defun(cog_object*, cog_object*);

/**
 * Creates a variable object from a value. Variable objects
 * are special objects that when run will do nothing but push
 * their value.
 */
cog_object* cog_make_var(cog_object*);

/**
 * Gets a defined function from the current scope(s).
 * @param identifier The identifier of the function.
 * @param found A pointer to a boolean that will be set to true
 * if the function was defined, or false if it is not defined.
 * @return The value of the function. May be NULL.
 */
cog_object* cog_get_fun(cog_object* identifier, bool* found);

/**
 * Pushes a new empty scope onto the scope stack.
 */
void cog_push_new_scope();

/**
 * Pushes the saved scope onto the scope stack.
 */
void cog_push_scope(cog_object*);

/**
 * Pops the current scope from the scope stack.
 */
void cog_pop_scope();

/**
 * Hashes an object.
 * @param obj The object to hash.
 * @return The hash value.
 */
cog_object* cog_hash(cog_object* obj);

extern cog_obj_type cog_ot_pointer;
extern cog_obj_type cog_ot_owned_pointer;
extern cog_obj_type cog_ot_symbol;
extern cog_obj_type cog_ot_identifier;
extern cog_obj_type cog_ot_string;
extern cog_obj_type cog_ot_int;
extern cog_obj_type cog_ot_bool;
extern cog_obj_type cog_ot_float;
extern cog_obj_type cog_ot_boolean;
extern cog_obj_type cog_ot_continuation;

/**
 * Returns early with an error status and the specified message.
 */
#define COG_RETURN_ERROR(msg) \
    do { \
        cog_push(msg); \
        return cog_error(); \
    } while (0)

/**
 * Runs the specified well-known method on an object, returning early with an error status if it fails.
 */
#define COG_RUN_WKM_RETURN_IF_ERROR(obj, method) \
    do { \
        cog_object* result__ = cog_run_well_known((obj), (method)); \
        if (cog_same_identifiers(result__, cog_error())) return result__; \
    } while (0)

#define COG_MAYBE_DATA(val) ((val) != NULL ? (val)->data : NULL)

/**
 * Iterates over a list.
 */
#define COG_ITER_LIST(list, var) \
    for (cog_object* head__ = (list), *var = COG_MAYBE_DATA(head__); head__ != NULL; head__ = head__->next, var = COG_MAYBE_DATA(head__))

/**
 * Ensures that there are at least `n` items on the stack, returning early with an error if not.
 */
#define COG_ENSURE_N_ITEMS(n) \
    do { \
        if (!cog_stack_has_at_least(n)) { \
            COG_RETURN_ERROR(cog_sprintf("%s: Expected %d items on the stack, but there were only %d", __func__, n, cog_stack_length())); \
        } \
    } while (0)

/**
 * Ensures that an object is of a specific type, returning early with an error if not.
 */
#define COG_ENSURE_TYPE(obj, typeobj) \
    do { \
        if ((obj) == NULL || (obj)->type != typeobj) { \
            COG_RETURN_ERROR(cog_sprintf("Expected %s, but got %s", \
                (typeobj) ? (typeobj)->typename : "NULL", (obj) && (obj)->type ? (obj)->type->typename : "NULL")); \
        } \
    } while (0)

/**
 * Ensures that an object is a list type, returning early with an error if not.
 */
#define COG_ENSURE_LIST(obj) \
    do { \
        if ((obj) && (obj)->type) { \
            COG_RETURN_ERROR(cog_sprintf("Expected list, but got %s", (obj) ? (obj)->type->typename : "NULL")); \
        } \
    } while (0)

/**
 * Ensures that an object is one of the numeric types, returning early with an error if not, and storing the float result in the variable
 */
#define COG_GET_NUMBER(obj, var) \
    do { \
        if ((obj) == NULL || ((obj)->type != &cog_ot_int && (obj)->type != &cog_ot_float)) { \
            COG_RETURN_ERROR(cog_sprintf("Expected a number, but got %s", (obj) ? (obj)->type->typename : "NULL")); \
        } \
        else var = (obj)->type == &cog_ot_float ? (obj)->as_float : (obj)->as_int; \
    } while (0)

#ifdef __cplusplus
}
#endif
