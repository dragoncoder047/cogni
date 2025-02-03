#define _FORTIFY_SOURCE 3
#include "cog_regex.h"
#include <regex.h>

#define _REGEX_HEADER \
    COG_ENSURE_N_ITEMS(2); \
    cog_object* pat = cog_pop(); \
    cog_object* str = cog_pop(); \
    COG_ENSURE_TYPE(pat, &cog_ot_string); \
    COG_ENSURE_TYPE(str, &cog_ot_string); \
    char* pbuf = (char*)alloca(cog_strlen(pat) + 1); \
    char* sbuf = (char*)alloca(cog_strlen(str) + 1); \
    cog_string_to_cstring(pat, pbuf, SIZE_MAX); \
    cog_string_to_cstring(str, sbuf, SIZE_MAX); \
    regex_t reg; \
    int status = regcomp(&reg, pbuf, REG_EXTENDED | REG_NEWLINE); \
	errno = 0; /* Hmmm */ \
    if (status != 0) { \
        size_t size = regerror(status, &reg, NULL, 0); \
        char* errmsg = (char*)alloca(size + 1); \
		regerror(status, &reg, errmsg, size); \
        regfree(&reg); \
        COG_RETURN_ERROR(cog_sprintf("Error compiling regex %O: %s", pat, errmsg)); \
    }

#define _REGEX_RUN_ERR \
    if (status && status != REG_NOMATCH) { \
        size_t size = regerror(status, &reg, NULL, 0); \
        char* errmsg = (char*)alloca(size + 1); \
		regerror(status, &reg, errmsg, size); \
        regfree(&reg); \
        COG_RETURN_ERROR(cog_sprintf("Error running regex %O on string %O: %s", pat, str, errmsg)); \
    }

cog_object* fn_regex() {
    _REGEX_HEADER
    status = regexec(&reg, sbuf, 0, NULL, 0);
    _REGEX_RUN_ERR
    regfree(&reg);
    cog_push(cog_box_bool(status != REG_NOMATCH));
    return NULL;
}
cog_modfunc fne_regex = {"Regex", COG_FUNC, fn_regex, "Tests if the regex matches the string at any point."};

cog_object* fn_regexmatch() {
    _REGEX_HEADER
    size_t groups = reg.re_nsub + 1;
	regmatch_t matches[groups];
    status = regexec(&reg, sbuf, groups, matches, 0);
    _REGEX_RUN_ERR
    if (status == 0) {
        for (int g = 1; g < groups; g++) {
			if (matches[g].rm_so == -1) {
				groups = g;
				break;
			}
		}
		for (int g = groups - 1; g > 0; g--) {
			size_t from = matches[g].rm_so;
			size_t to = matches[g].rm_eo;
			cog_push(cog_substring(str, from, to));
		}
    }
    regfree(&reg);
    cog_push(cog_box_bool(status != REG_NOMATCH));
    return NULL;
}
cog_modfunc fne_regexmatch = {"Regex-Match", COG_FUNC, fn_regexmatch, "Matches the regex to the string, and if it matches returns true along with the capturing groups under it, otherwise false."};

cog_modfunc* m_regex_functions[] = {
    &fne_regex,
    &fne_regexmatch,
    NULL
};

cog_module m_regex = {"Regex", m_regex_functions, NULL, NULL};
