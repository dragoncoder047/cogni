#include <stdio.h>
#include <stdlib.h>

#include "cogni.h"
#include "files.h"
#include "prelude.inc"

cog_object* fn_test() {
    cog_printf("Hello, World! My cookie is %O\n", cog_pop());
    return NULL;
}
cog_modfunc af_test = {"Test-builtin", COG_FUNC, fn_test, "test functionality"};

cog_modfunc* m_test_table[] = {
    &af_test,
    NULL
};

cog_module test = {"Test", m_test_table, NULL, NULL};

bool run(cog_object* what, cog_object* cookie) {
    cog_run_next(what, NULL, cookie);
    cog_object* end_status = cog_mainloop(NULL);

    if (cog_same_identifiers(end_status, cog_error())) {
        cog_object* msg = cog_pop();
        cog_printf("ERROR: %#O\n", msg);
        return false;
    } else {
        // cog_object* done = cog_pop();
        // cog_push(done);
        // cog_printf("DEBUG: result is %#O\n", done);
        return true;
    }
}

void repl() {
    printf("ERROR: REPL not implemented yet\n");
    abort();
}

void usage() {
    printf("Usage: cogni [filename]\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
    cog_init();
    cog_add_module(&test);
    cog_add_module(&m_file);
    cog_set_stdout(cog_open_file("/dev/stdout", "w"));
    cog_set_stdin(cog_open_file("/dev/stdin", "r"));
    cog_set_stderr(cog_open_file("/dev/stderr", "w"));


    // parse the prelude
    cog_push(cog_string_from_bytes((char*)cognac_src_prelude_cog, cognac_src_prelude_cog_len));
    if (!run(cog_make_identifier_c("Parse"), NULL)) {
        printf("Error parsing prelude\n");
        goto end;
    }
    // run the block to make it into a closure
    if (!run(cog_pop(), NULL)) {
        printf("Error processing prelude\n");
        goto end;
    }
    // Then run the closure
    if (!run(cog_pop(), cog_box_bool(false))) {
        printf("Error running prelude\n");
        goto end;
    }

    // Run user script

    if (argc == 1) repl();
    else if (argc == 2) {
        char* filename = argv[1];
        cog_push(cog_open_file(filename, "r"));
        if (errno == ENOENT) {
            fprintf(stderr, "Error: file %s not found\n", filename);
            goto end;
        }
    } else usage();

    if (!run(cog_make_identifier_c("Parse"), NULL)) {
        printf("Error parsing user script\n");
        goto end;
    }
    if (!run(cog_pop(), NULL)) {
        printf("Error processing user script\n");
        goto end;
    }
    if (!run(cog_pop(), cog_box_bool(false))) {
        printf("Error running user script\n");
        goto end;
    }
    end:
    cog_quit();
    return 0;
}
