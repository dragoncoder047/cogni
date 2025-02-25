#define _FORTIFY_SOURCE 3
#include "files.h"
#include <stdio.h>
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
cog_object_method ome_file_write = {&ot_file, "Stream::PutString", m_file_write};

static cog_object* m_file_getch() {
    cog_object* file = cog_pop();
    FILE* f = (FILE*)file->as_ptr;
    if (feof(f)) cog_push(cog_eof());
    else cog_push(cog_make_character(fgetc(f)));
    return NULL;
}
static cog_object_method ome_file_getch = {&ot_file, "Stream::GetChar", m_file_getch};

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
static cog_object_method ome_file_ungets = {&ot_file, "Stream::UngetString", m_file_ungets};

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
cog_object_method ome_file_stringify = {&ot_file, "Show", m_file_stringify};

static cog_object* m_file_get_name() {
    cog_object* file = cog_pop();
    cog_push(file->next);
    return NULL;
}
cog_object_method ome_file_get_name = {&ot_file, "Stream::Get_Name", m_file_get_name};

cog_object_method ome_file_hash = {&ot_file, "Hash", cog_not_implemented};

cog_object_method* m_file_table[] = {
    &ome_file_write,
    &ome_file_getch,
    &ome_file_ungets,
    &ome_file_stringify,
    &ome_file_get_name,
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
    errno = 0;
    FILE* f = fopen(buf, mod);
    if (errno) COG_RETURN_ERROR(cog_sprintf("While opening %O: [Errno %i] %s", filename, errno, strerror(errno)));
    else if (!f) COG_RETURN_ERROR(cog_sprintf("Unknown error while opening %O", filename));
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
    cog_object* str = cog_emptystring();
    cog_object* tail = str;
    for (int ch = fgetc(f); ch != EOF; ch = fgetc(f))
        cog_string_append_byte(&tail, ch);
    cog_push(str);
    return NULL;
}
cog_modfunc fne_readfile = {"Read-File", COG_FUNC, fn_readfile, "Read the entire contents of the file into a string, from beginning to end."};

cog_object* fn_seek() {
    COG_ENSURE_N_ITEMS(3);
    cog_object* how = cog_pop();
    cog_object* where = cog_pop();
    cog_object* what = cog_pop();
    COG_ENSURE_TYPE(how, &cog_ot_symbol);
    float n = 0;
    COG_GET_NUMBER(where, n);
    COG_ENSURE_TYPE(what, &ot_file);
    int w = SEEK_SET;
    FILE* f = (FILE*)what->as_ptr;
    if (!f) COG_RETURN_ERROR(cog_string("Tried to seek a closed file"));
    if (cog_same_identifiers(how->next, cog_make_identifier_c("start"))) w = SEEK_SET;
    else if (cog_same_identifiers(how->next, cog_make_identifier_c("end"))) w = SEEK_END;
    else if (cog_same_identifiers(how->next, cog_make_identifier_c("current"))) w = SEEK_CUR;
    else COG_RETURN_ERROR(cog_sprintf("Expected one of \\start, \\end, \\current but got %O", how));
    long nf = n;
    if (nf != n) COG_RETURN_ERROR(cog_sprintf("can't seek to a non-integer offset: %O", where));
    int err = fseek(f, n, w);
    if (err) COG_RETURN_ERROR(cog_sprintf("failed to seek to %O for %O", where, how));
    return NULL;
}
cog_modfunc fne_seek = {"Seek", COG_FUNC, fn_seek, "Seeks a file to a particular offset."};

cog_object* fn_readline() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* file = cog_pop();
    COG_ENSURE_TYPE(file, &ot_file);
    FILE* f = (FILE*)file->as_ptr;
    if (!f) COG_RETURN_ERROR(cog_string("Tried to read a closed file"));
    cog_object* str = cog_emptystring();
    cog_object* tail = str;
    int ch;
    do {
        ch = fgetc(f);
        cog_string_append_byte(&tail, ch);
    } while (ch != '\n');
    cog_push(str);
    return NULL;
}
cog_modfunc fne_readline = {"Read-Line", COG_FUNC, fn_readline, "Read a line of text from the file, until and including the next newline (ASCII 0x0A) character."};

cog_object* fn_close() {
    COG_ENSURE_N_ITEMS(1);
    cog_object* file = cog_pop();
    COG_ENSURE_TYPE(file, &ot_file);
    FILE* f = (FILE*)file->as_ptr;
    if (!f) COG_RETURN_ERROR(cog_string("File is already closed"));
    if (f != stdin && f != stdout && f != stderr) {
        fclose(f);
    }
    file->as_ptr = NULL;
    return NULL;
}
cog_modfunc fne_close = {"Close", COG_FUNC, fn_close, "Close an opened file."};

cog_modfunc* m_file_functions[] = {
    &fne_open,
    &fne_readfile,
    &fne_readline,
    &fne_seek,
    &fne_close,
    NULL
};

cog_module m_file = {"IO::FILE", m_file_functions, m_file_table, m_file_types};
