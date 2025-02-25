#define _FORTIFY_SOURCE 3
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "cogni.h"
#include "files.h"
#include "cog_regex.h"
#include "misc_io.h"
#include "prelude.inc"
#include "prelude2.inc"

bool do_top(cog_object* cookie) {
    cog_run_next(cog_pop(), NULL, cookie);
    cog_object* end_status = cog_mainloop(NULL);

    if (cog_same_identifiers(end_status, cog_error())) {
        cog_object* msg = cog_pop();
        if (msg) cog_printf("ERROR: %#O\n", msg);
        if (isatty(fileno(stdout))) putchar('\a');
        return false;
    }
    return true;
}

bool run(cog_object* obj, const char* what) {
    // parse it
    if (obj->type == &cog_ot_string) {
        obj = cog_iostring_wrap(obj);
        cog_iostring_set_name(obj, cog_string(what));
    }
    cog_push(obj);
    cog_push(cog_make_identifier_c("Parse"));
    if (!do_top(NULL)) return false;
    // run the block to make it into a closure
    if (!do_top(NULL)) return false;
    // Then run the closure
    if (!do_top(cog_box_bool(false))) return false;
    return true;
}

static jmp_buf interrupt_jump;
static void interrupt(int sig) {
    longjmp(interrupt_jump, 1);
}
void repl() {
    printf("use ^D to exit REPL\nWARNING: REPL is buggy\n");
    rl_initialize();
    using_history();
    signal(SIGINT, interrupt);
    char* line_input;
    cog_object* the_string = cog_emptystring();
    const char* prompt = "cognate> ";
    for (;;) {
        bool is_end = false;
        if (!setjmp(interrupt_jump)) {
            line_input = readline(prompt);
            prompt = "    ...> ";
            if (!line_input) {
                putchar('\n');
                return;
            }
            // add newline at end of input line cause
            // readline doesn't do that automatically
            size_t len = strlen(line_input);
            if (len){
                line_input = (char*)realloc(line_input, len + 1);
                line_input[len] = '\n';
                line_input[len+1] = 0;
                cog_strcat(&the_string, cog_string(line_input));
                add_history(line_input);
            }
            free(line_input);
            if (!len) {
                is_end = true;
                run(the_string, "<input>");
            }
        } else {
            cog_push(cog_string("Interrupted!"));
            cog_push(cog_make_identifier_c("Error"));
            do_top(NULL);
        }
        if (is_end) {
            the_string = cog_emptystring();
            prompt = "cognate> ";
            cog_object* the_stack = cog_get_stack();
            if (the_stack) cog_printf("Stack: %O\n", the_stack);
            else printf("Stack empty\n");
        }
    }
}

void usage(const char* argv0) {
    printf("Usage: %s [filename]\n", argv0);
    printf("   or: %s -c \"commands\"\n", argv0);
    cog_quit();
    exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
    cog_init();
    cog_add_module(&m_file);
    cog_add_module(&m_misc_io);
    cog_add_module(&m_regex);
    cog_set_stdout(cog_open_file("/dev/stdout", "w"));
    cog_set_stdin(cog_open_file("/dev/stdin", "r"));
    cog_set_stderr(cog_open_file("/dev/stderr", "w"));

    // push parameters
    cog_object* params = NULL;
    for (int i = argc - 1; i >= 0; i--) {
        cog_push_to(&params, cog_string(argv[i]));
    }
    cog_defun(cog_make_identifier_c("Parameters"), cog_make_var(params));

    cog_object* prelude = cog_string_from_bytes((char*)cognac_src_prelude_cog, cognac_src_prelude_cog_len);
    cog_object* userscript = NULL;
    if (!run(prelude, "<prelude>")) goto end;
    prelude = cog_string_from_bytes((char*)prelude2_cog, prelude2_cog_len);
    if (!run(prelude, "<prelude2>")) goto end;

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

    run(userscript, "<string>");
    end:
    cog_quit();
    return 0;
}
