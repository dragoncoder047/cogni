#define _FORTIFY_SOURCE 3
#include "files.h"
#include <stdio.h>
#include <unistd.h>

cog_object* fn_path() {
    char buf[FILENAME_MAX];
    errno = 0;
	if (!getcwd(buf, FILENAME_MAX)) COG_RETURN_ERROR(cog_sprintf("Couldn't get current path: [Errno %i] %s", errno, strerror(errno)));
    cog_push(cog_string(buf));
    return NULL;
}
cog_modfunc fne_path = {"Path", COG_FUNC, fn_path, "Return the current working directory."};

#if __has_include(<readline/readline.h>)
#define USE_READLINE 1
#include <readline/readline.h>
#else
#define USE_READLINE 0
#endif

cog_object* fn_input() {
    // TODO: use cog_get_stdin()
    char* input;
    #if USE_READLINE
    if (isatty(fileno(stdin))) {
        input = readline("");
        goto gotten;
    }
    #endif
    size_t len;
	getline(&input, &len, stdin);
    gotten:
    cog_push(cog_string(input));
    free(input);
    return NULL;
}
cog_modfunc fne_input = {"Input", COG_FUNC, fn_input, "Return a line of user input form stdin."};

cog_modfunc* m_misc_io_functions[] = {
    &fne_path,
    &fne_input,
    NULL
};

cog_module m_misc_io = {"IO::Misc", m_misc_io_functions, NULL, NULL};
