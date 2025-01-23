#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char** argv) {
    puts("cogni!");
    cog_init();
    cog_add_module(&test);

    // test values to see if stuff is getting popped too much
    for (cog_integer i = 0; i < 10; i++) cog_push(cog_box_int(i));

    cog_object* s = cog_string_from_bytes((char*)prelude_cog, prelude_cog_len);

    cog_push(s);
    cog_run_next(cog_make_identifier_c("Print"), NULL, NULL);
    cog_run_next(cog_make_identifier_c("Parse"), NULL, NULL);

    cog_object* end = cog_mainloop(NULL);

    if (cog_same_identifiers(end, cog_error())) {
        cog_object* msg = cog_pop();
        cog_printf("Exit ERROR: %#O\n", msg);
    }

    printf("%zu cells used at exit\n", cog_get_num_cells_used());

    cog_quit();
    return 0;
}
