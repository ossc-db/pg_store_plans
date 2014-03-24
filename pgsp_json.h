#include "pgsp_json_text.h"

extern char *pgsp_json_normalize(char *json);
extern char *pgsp_json_shorten(char *json);
extern char *pgsp_json_inflate(char *json);
extern char *pgsp_json_yamlize(char *json);
extern char *pgsp_json_xmlize(char *json);
extern void normalize_expr(char *expr, bool preserve_space);
