#define _FORTIFY_SOURCE 3
#include "files.h"
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>

static void closefile(cog_object* stream) {
    if (stream->as_ptr) {
        FILE* f = (FILE*)stream->as_ptr;
        if (f != stdin && f != stdout && f != stderr) {
            fclose(f);
            stream->as_ptr = NULL;
        }
    }
}
cog_obj_type ot_file = {"File", cog_walk_only_next, closefile};

cog_object* cog_make_filestream(FILE* f, cog_object* filename) {
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
    cog_object* buf = cog_pop();
    FILE* f = (FILE*)file->as_ptr;
    while (buf) {
        fprintf(f, "%.*s", buf->stored_chars, buf->as_chars);
        fflush(f);
        buf = buf->next;
    }
    return NULL;
}
cog_object_method ome_file_write = {&ot_file, COG_SM_PUTS, m_file_write};

static cog_object* m_file_getch() {
    cog_object* file = cog_pop();
    FILE* f = (FILE*)file->as_ptr;
    if (feof(f)) cog_push(cog_eof());
    else cog_push(cog_make_character(fgetc(f)));
    return NULL;
}
static cog_object_method ome_file_getch = {&ot_file, COG_SM_GETCH, m_file_getch};

static cog_object* m_file_ungets() {
    cog_object* file = cog_pop();
    cog_object* buf = cog_pop();
    FILE* f = (FILE*)file->as_ptr;
    while (buf) {
        for (int i = 0; i < buf->stored_chars; i++)
            ungetc(buf->as_chars[i], f);
        buf = buf->next;
    }
    return NULL;
}
static cog_object_method ome_file_ungets = {&ot_file, COG_SM_UNGETS, m_file_ungets};

/*
XXX:                 then why does this not work??
              ┌──────────────┬───────────────────────────────┐
              │ fopen() mode │ open() flags                  │
              ├──────────────┼───────────────────────────────┤
              │      r       │ O_RDONLY                      │
              ├──────────────┼───────────────────────────────┤
              │      w       │ O_WRONLY | O_CREAT | O_TRUNC  │
              ├──────────────┼───────────────────────────────┤
              │      a       │ O_WRONLY | O_CREAT | O_APPEND │
              ├──────────────┼───────────────────────────────┤
              │      r+      │ O_RDWR                        │
              ├──────────────┼───────────────────────────────┤
              │      w+      │ O_RDWR | O_CREAT | O_TRUNC    │
              ├──────────────┼───────────────────────────────┤
              │      a+      │ O_RDWR | O_CREAT | O_APPEND   │
              └──────────────┴───────────────────────────────┘
*/

static cog_object* m_file_stringify() {
    cog_object* file = cog_pop();
    FILE* f = (FILE*)file->as_ptr;
    int modes = fcntl(fileno(f), F_GETFL);
    const char* modestr;
    switch (modes) {
        case O_RDONLY: modestr = "read"; break;
        case O_WRONLY | O_CREAT | O_TRUNC: modestr = "write"; break;
        case O_WRONLY | O_CREAT | O_APPEND: modestr = "append"; break;
        case O_RDWR: modestr = "read-write-existing"; break;
        case O_RDWR | O_CREAT | O_TRUNC: modestr = "read-write"; break;
        case O_RDWR | O_CREAT | O_APPEND: modestr = "read-append"; break;
        default: modestr = "???"; break;
    }
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<File %O at pos %li mode %s>", file->next, ftell(f), modestr));
    return NULL;
}
cog_object_method ome_file_stringify = {&ot_file, COG_M_SHOW, m_file_stringify};

cog_object_method ome_file_hash = {&ot_file, COG_M_HASH, cog_not_implemented};

cog_object_method* m_file_table[] = {
    &ome_file_write,
    &ome_file_getch,
    &ome_file_ungets,
    &ome_file_stringify,
    &ome_file_hash,
    NULL
};

cog_obj_type* m_file_types[] = {
    &ot_file,
    NULL
};

cog_object* fn_open() {
    COG_ENSURE_N_ITEMS(2);
    cog_object* mode = cog_pop();
    cog_object* filename = cog_pop();
    COG_ENSURE_TYPE(mode, &cog_ot_symbol);
    COG_ENSURE_TYPE(filename, &cog_ot_string);
    const char* mod;
    // get the mode
    if (cog_same_identifiers(mode->next, cog_make_identifier_c("read"))) mod = "r";
    else if (cog_same_identifiers(mode->next, cog_make_identifier_c("write"))) mod = "w";
    else if (cog_same_identifiers(mode->next, cog_make_identifier_c("append"))) mod = "a";
    else if (cog_same_identifiers(mode->next, cog_make_identifier_c("read-append"))) mod = "a+";
    else if (cog_same_identifiers(mode->next, cog_make_identifier_c("read-write"))) mod = "w+";
    else if (cog_same_identifiers(mode->next, cog_make_identifier_c("read-write-existing"))) mod = "r+";
    else COG_RETURN_ERROR(cog_sprintf("Expected one of \\read, \\write, \\append, \\read-write, \\read-append, \\read-write-existing but got %O", mode));
    // get the filename
    size_t len = cog_strlen(filename);
    char* buf = (char*)alloca(len + 1);
    memset(buf, 0, len + 1);
    cog_string_to_cstring(filename, buf, len);
    FILE* f = fopen(buf, mod);
    if (errno) COG_RETURN_ERROR(cog_sprintf("While opening %O: [Errno %i] %s", filename, errno, strerror(errno)));
    cog_push(cog_make_filestream(f, filename));
    return NULL;
}
cog_modfunc fne_open = {"Open", COG_FUNC, fn_open, "Open a file with a specific mode."};

cog_object* fn_readfile() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* file = cog_pop();
    COG_ENSURE_TYPE(file, &ot_file);
    FILE* f = (FILE*)file->as_ptr;
    if (!f) COG_RETURN_ERROR(cog_string("Tried to read a closed file"));
    fseek(f, 0, SEEK_SET);
    struct stat st;
	fstat(fileno(f), &st);
    char* b = (char*)alloca(st.st_size + 1);
    memset(b, 0, st.st_size + 1);
    size_t read = fread(b, sizeof(char), st.st_size, f);
    if (read != st.st_size) COG_RETURN_ERROR(cog_sprintf("Couldn't read file %O", file->next));
    b[st.st_size] = 0; // remove trailing EOF
    cog_push(cog_string(b));
    return NULL;
}
cog_modfunc fne_readfile = {"Read-file", COG_FUNC, fn_readfile, "Read the entire contents of the file into a string."};

cog_modfunc* m_file_functions[] = {
    &fne_open,
    &fne_readfile,
    NULL
};

cog_module m_file = {"IO::FILE", m_file_functions, m_file_table, m_file_types};
