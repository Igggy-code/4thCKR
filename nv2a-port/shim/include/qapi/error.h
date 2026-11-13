#pragma once
typedef struct Error Error;
#define error_setg(errp, ...) (fprintf(stderr, __VA_ARGS__), fputc(10, stderr))
#define error_fatal NULL
#define error_abort NULL
#define error_prepend(errp, ...) ((void)(errp))
#define error_propagate(a,b) ((void)(a),(void)(b))
#define error_free(e) ((void)(e))
#define error_get_pretty(e) "error"
