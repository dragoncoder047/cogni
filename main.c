#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cogni.h"
#include "prelude.h"

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

int main(int argc, char** argv) {
    cog_init();
    cog_add_module(&test);

    // if (argc == 0) repl();
    // else {
    //
    // }
    cog_object* s = cog_string_from_bytes((char*)prelude_cog, prelude_cog_len);


    clock_t start_ticks = clock();
    cog_push(s);
    // parse the prelude
    if (!run(cog_make_identifier_c("Parse"), NULL)) {
        fprintf(stderr, "Failed to parse prelude\n");
        goto end;
    }
    // run the block to make it into a closure
    if (!run(cog_pop(), NULL)) {
        fprintf(stderr, "Failed to close prelude\n");
        goto end;
    }
    // Then run the closure
    if (!run(cog_pop(), cog_box_bool(false))) {
        fprintf(stderr, "Failed to run prelude\n");
        goto end;
    }

    // Run user script
    cog_push(cog_string("Test-builtin"));
    if (!run(cog_make_identifier_c("Parse"), NULL)) {
        fprintf(stderr, "Failed to parse user script\n");
        goto end;
    }
    if (!run(cog_pop(), NULL)) {
        fprintf(stderr, "Failed to close user script\n");
        goto end;
    }
    if (!run(cog_pop(), cog_box_bool(false))) {
        fprintf(stderr, "Failed to run user script\n");
        goto end;
    }
    end:;
    clock_t end_ticks = clock();

    printf("%zu cells used at exit\n", cog_get_num_cells_used());
    printf("Execution time: %f seconds\n", (double)(end_ticks - start_ticks) / CLOCKS_PER_SEC);

    cog_quit();
    return 0;
}
