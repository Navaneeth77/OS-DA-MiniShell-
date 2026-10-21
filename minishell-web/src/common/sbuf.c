/* sbuf.c - growable strings, string lists and JSON helpers. */
#include "sbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { perror("realloc"); exit(1); }
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void sb_reserve(sbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void sb_init(sbuf *b) {
    b->data = xmalloc(64);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = 64;
}

void sb_free(sbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void sb_clear(sbuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void sb_append(sbuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(sbuf *b, const char *s) { sb_append(b, s, strlen(s)); }

void sb_putc(sbuf *b, char c) { sb_append(b, &c, 1); }

void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n > 0) {
        sb_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
        b->len += (size_t)n;
    }
    va_end(ap);
}

void sb_json_str(sbuf *b, const char *s, size_t n) {
    if (!s) { sb_puts(b, "null"); return; }
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (c < 0x20 || c == 0x7f) sb_printf(b, "\\u%04x", c);
            else sb_putc(b, (char)c);
        }
    }
    sb_putc(b, '"');
}

void sb_json_cstr(sbuf *b, const char *s) { sb_json_str(b, s, s ? strlen(s) : 0); }

void sl_push(strlist *l, char *owned) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, (size_t)l->cap * sizeof *l->items);
    }
    l->items[l->count++] = owned;
}

void sl_free(strlist *l) {
    for (int i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}
