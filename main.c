#define _FORTIFY_SOURCE 3
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "cogni.h"
#include "files.h"
#include "prelude.inc"
#include "prelude2.inc"

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

bool do_top(cog_object* cookie, cog_object* status) {
    cog_run_next(cog_pop(), NULL, cookie);
    cog_object* end_status = cog_mainloop(status);

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

bool run(cog_object* obj) {
    // parse it
    cog_push(obj);
    cog_push(cog_make_identifier_c("Parse"));
    if (!do_top(NULL, NULL)) return false;
    // run the block to make it into a closure
    if (!do_top(NULL, NULL)) return false;
    // Then run the closure
    if (!do_top(cog_box_bool(false), NULL)) return false;
    return true;
}

static jmp_buf interrupt_jump;
static void interrupt(int sig) {
    printf("\b\b  \b\b"); // clear the "^C"
    fflush(stdout);
    longjmp(interrupt_jump, 1);
}
void repl() {
    printf("use Stop to exit REPL\nWARNING: REPL is buggy\n");
    rl_initialize();
    signal(SIGINT, interrupt);
    char* line_input;
    cog_object* the_string = cog_emptystring();
    const char* prompt = "cognate> ";
    for (;;) {
        line_input = readline(prompt);
        prompt = "    ...> ";
        cog_strcat(&the_string, cog_string(line_input));
        size_t len = strlen(line_input);
        if (len) add_history(line_input);
        free(line_input);
        if (!len) {
            if (!setjmp(interrupt_jump)) {
                run(the_string);
            } else {
                cog_push(cog_string("Interrupted!"));
                do_top(NULL, cog_error());
            }
            the_string = cog_emptystring();
            prompt = "cognate> ";
        }
    }
}

void usage(const char* argv0) {
    printf("Usage: %s [filename]\n", argv0);
    printf("   or: %s -c \"commands\"\n", argv0);
    exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
    cog_init();
    cog_add_module(&test);
    cog_add_module(&m_file);
    cog_set_stdout(cog_open_file("/dev/stdout", "w"));
    cog_set_stdin(cog_open_file("/dev/stdin", "r"));
    cog_set_stderr(cog_open_file("/dev/stderr", "w"));

    cog_object* prelude = cog_string_from_bytes((char*)cognac_src_prelude_cog, cognac_src_prelude_cog_len);
    cog_object* userscript = NULL;
    if (!run(prelude)) goto end;
    prelude = cog_string_from_bytes((char*)prelude2_cog, prelude2_cog_len);
    if (!run(prelude)) goto end;

    // Run user script
    if (argc == 1) {
        repl();
        goto end;
    }
    else if (argc == 2) {
        char* filename = argv[1];
        userscript = cog_open_file(filename, "r");
        if (errno) {
            fprintf(stderr, "%s: %s: %s\n", argv[0], filename, strerror(errno));
            goto end;
        }
    } else if (argc == 3 && !strcmp(argv[1], "-c")) {
        userscript = cog_string(argv[2]);
    } else usage(argv[0]);

    run(userscript);
    end:
    cog_quit();
    return 0;
}
