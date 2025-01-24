#pragma once

#include "cogni.h"

/**
 * Creates a file stream from a `FILE*` pointer.
 * @param filename The filename associated with the file stream, or `NULL` if it is not known.
 */
cog_object* cog_make_filestream(FILE*, cog_object*);

cog_object* cog_open_file(const char* const filename, const char* const mode);

extern cog_module m_file;
