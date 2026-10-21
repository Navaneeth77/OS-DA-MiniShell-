/* sbuf.h - growable strings, string lists and JSON helpers. */
#ifndef SBUF_H
#define SBUF_H

#include <stddef.h>

typedef struct {
    char *data;     /* always NUL-terminated */
    size_t len;
    size_t cap;
} sbuf;

typedef struct {
    char **items;
    int count;
    int cap;
} strlist;

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_clear(sbuf *b);
void sb_append(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_putc(sbuf *b, char c);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Appends a JSON string literal ("...") with proper escaping. NULL -> null. */
void sb_json_str(sbuf *b, const char *s, size_t n);
void sb_json_cstr(sbuf *b, const char *s);

void sl_push(strlist *l, char *owned);
void sl_free(strlist *l);

#endif
