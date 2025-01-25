#define _FORTIFY_SOURCE 3
#include "files.h"

static void closefile(cog_object* stream) {
    if (stream->as_ptr) {
        FILE* f = stream->as_ptr;
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
    FILE* f = file->as_ptr;
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
    FILE* f = file->as_ptr;
    if (feof(f)) cog_push(cog_eof());
    else cog_push(cog_make_character(fgetc(f)));
    return NULL;
}
static cog_object_method ome_file_getch = {&ot_file, COG_SM_GETCH, m_file_getch};

static cog_object* m_file_ungets() {
    cog_object* file = cog_pop();
    cog_object* buf = cog_pop();
    FILE* f = file->as_ptr;
    while (buf) {
        for (int i = 0; i < buf->stored_chars; i++)
            ungetc(buf->as_chars[i], f);
        buf = buf->next;
    }
    return NULL;
}
static cog_object_method ome_file_ungets = {&ot_file, COG_SM_UNGETS, m_file_ungets};

static cog_object* m_file_stringify() {
    cog_object* file = cog_pop();
    cog_pop(); // ignore readably
    cog_push(cog_sprintf("<File %O at pos %li>", file->next, ftell((FILE*)file->as_ptr)));
    return NULL;
}
cog_object_method ome_file_stringify = {&ot_file, COG_M_SHOW, m_file_stringify};

cog_object_method* m_file_table[] = {
    &ome_file_write,
    &ome_file_getch,
    &ome_file_ungets,
    &ome_file_stringify,
    NULL
};

cog_obj_type* m_file_types[] = {
    &ot_file,
    NULL
};

cog_module m_file = {"IO::FILE", NULL, m_file_table, m_file_types};
