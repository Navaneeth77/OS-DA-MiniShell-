/* parser.c - tokenizer and parser.
 *
 * Stage 1 (tokenize): characters -> words and operators. Handles 'single'
 *   and "double" quotes, backslash escapes, $VAR ${VAR} $? $$, ~ and # comments.
 * Stage 2 (parse_line): tokens -> commands separated by '|', each with its
 *   redirections, plus a trailing '&' for background execution.
 */
#include "parser.h"
#include "sbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { T_WORD, T_PIPE, T_AMP, T_IN, T_OUT, T_APPEND, T_DUP } tok_type;

typedef struct {
    tok_type type;
    char *text;      /* T_WORD only */
    int fd;
    int src_fd;
} token;

typedef struct {
    token *items;
    int count;
    int cap;
} token_list;

static void tl_push(token_list *l, tok_type type, char *text, int fd, int src_fd) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, (size_t)l->cap * sizeof *l->items);
    }
    l->items[l->count++] = (token){type, text, fd, src_fd};
}

static void tl_free(token_list *l) {
    for (int i = 0; i < l->count; i++) free(l->items[i].text);
    free(l->items);
}

static const char *tok_symbol(tok_type t) {
    switch (t) {
    case T_PIPE: return "|";
    case T_AMP: return "&";
    case T_IN: return "<";
    case T_OUT: return ">";
    case T_APPEND: return ">>";
    case T_DUP: return ">&";
    default: return "word";
    }
}

static int is_name_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Expands the reference at s[i] == '$'. Returns the index of the last
 * character consumed, or -1 on error. */
static long expand_var(const char *s, size_t n, size_t i, sbuf *out, int last_status,
                       char *err, size_t errlen) {
    char name[256];
    if (i + 1 >= n) { sb_putc(out, '$'); return (long)i; }
    char c = s[i + 1];
    if (c == '?') { sb_printf(out, "%d", last_status); return (long)i + 1; }
    if (c == '$') { sb_printf(out, "%d", (int)getpid()); return (long)i + 1; }
    if (c == '{') {
        const char *close = strchr(s + i + 2, '}');
        if (!close) { snprintf(err, errlen, "bad substitution: missing `}'"); return -1; }
        size_t len = (size_t)(close - (s + i + 2));
        if (len == 0 || len >= sizeof name || !is_name_start(s[i + 2])) {
            snprintf(err, errlen, "${...}: bad substitution");
            return -1;
        }
        memcpy(name, s + i + 2, len);
        name[len] = '\0';
        const char *v = getenv(name);
        if (v) sb_puts(out, v);
        return (long)(close - s);
    }
    if (is_name_start(c)) {
        size_t j = i + 1;
        while (j < n && is_name_char(s[j])) j++;
        size_t len = j - i - 1;
        if (len >= sizeof name) len = sizeof name - 1;
        memcpy(name, s + i + 1, len);
        name[len] = '\0';
        const char *v = getenv(name);
        if (v) sb_puts(out, v);
        return (long)j - 1;
    }
    sb_putc(out, '$');
    return (long)i;
}

static int tokenize(const char *line, int last_status, token_list *toks, char *err, size_t errlen) {
    size_t n = strlen(line);
    sbuf cur;
    sb_init(&cur);
    int in_word = 0;   /* set once the word has content, even an empty "" */

#define FLUSH()                                                      \
    do {                                                             \
        if (in_word) tl_push(toks, T_WORD, xstrdup(cur.data), -1, -1); \
        sb_clear(&cur);                                              \
        in_word = 0;                                                 \
    } while (0)

    for (size_t i = 0; i < n; i++) {
        char c = line[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { FLUSH(); continue; }
        if (c == '#' && !in_word) break;

        if (c == '\\') {
            if (i + 1 < n) sb_putc(&cur, line[++i]);
            in_word = 1;
            continue;
        }
        if (c == '\'') {
            const char *close = strchr(line + i + 1, '\'');
            if (!close) { snprintf(err, errlen, "unexpected EOF while looking for matching `''"); goto fail; }
            sb_append(&cur, line + i + 1, (size_t)(close - line) - i - 1);
            i = (size_t)(close - line);
            in_word = 1;
            continue;
        }
        if (c == '"') {
            int closed = 0;
            in_word = 1;
            for (i++; i < n; i++) {
                char d = line[i];
                if (d == '"') { closed = 1; break; }
                if (d == '\\' && i + 1 < n && strchr("\"\\$`", line[i + 1])) {
                    sb_putc(&cur, line[++i]);
                    continue;
                }
                if (d == '$') {
                    long r = expand_var(line, n, i, &cur, last_status, err, errlen);
                    if (r < 0) goto fail;
                    i = (size_t)r;
                    continue;
                }
                sb_putc(&cur, d);
            }
            if (!closed) { snprintf(err, errlen, "unexpected EOF while looking for matching `\"'"); goto fail; }
            continue;
        }
        if (c == '$') {
            size_t before = cur.len;
            long r = expand_var(line, n, i, &cur, last_status, err, errlen);
            if (r < 0) goto fail;
            i = (size_t)r;
            if (cur.len > before) in_word = 1;   /* unset variables vanish */
            continue;
        }
        if (c == '~' && !in_word && (i + 1 == n || line[i + 1] == '/' || isspace((unsigned char)line[i + 1]))) {
            const char *home = getenv("HOME");
            sb_puts(&cur, home ? home : "~");
            in_word = 1;
            continue;
        }

        /* Planned for the final DA. */
        if ((c == '&' || c == '|') && i + 1 < n && line[i + 1] == c) {
            snprintf(err, errlen, "`%c%c' is not supported yet", c, c);
            goto fail;
        }
        if (c == ';') { snprintf(err, errlen, "`;' is not supported yet"); goto fail; }

        if (c == '|') { FLUSH(); tl_push(toks, T_PIPE, NULL, -1, -1); continue; }
        if (c == '&') { FLUSH(); tl_push(toks, T_AMP, NULL, -1, -1); continue; }

        /* Redirections: [n]<  [n]>  [n]>>  [n]>&m */
        int digit_redirect = !in_word && isdigit((unsigned char)c) && i + 1 < n &&
                             (line[i + 1] == '>' || line[i + 1] == '<');
        if (c == '<' || c == '>' || digit_redirect) {
            FLUSH();
            int fd = -1, src = -1;
            tok_type type;
            if (digit_redirect) { fd = c - '0'; c = line[++i]; }
            if (c == '<') {
                type = T_IN;
                if (fd < 0) fd = 0;
            } else if (i + 1 < n && line[i + 1] == '>') {
                type = T_APPEND;
                i++;
            } else if (i + 2 < n && line[i + 1] == '&' && isdigit((unsigned char)line[i + 2])) {
                type = T_DUP;
                src = line[i + 2] - '0';
                i += 2;
            } else {
                type = T_OUT;
            }
            if (fd < 0) fd = 1;
            tl_push(toks, type, NULL, fd, src);
            continue;
        }

        sb_putc(&cur, c);
        in_word = 1;
    }
    FLUSH();
#undef FLUSH
    sb_free(&cur);
    return 0;

fail:
    sb_free(&cur);
    return -1;
}

static void cmd_add_arg(command *c, char *owned) {
    if (c->argc + 2 > c->argv_cap) {
        c->argv_cap = c->argv_cap ? c->argv_cap * 2 : 8;
        c->argv = xrealloc(c->argv, (size_t)c->argv_cap * sizeof *c->argv);
    }
    c->argv[c->argc++] = owned;
    c->argv[c->argc] = NULL;
}

static void cmd_add_redirect(command *c, redirect r) {
    if (c->nredirs == c->redirs_cap) {
        c->redirs_cap = c->redirs_cap ? c->redirs_cap * 2 : 4;
        c->redirs = xrealloc(c->redirs, (size_t)c->redirs_cap * sizeof *c->redirs);
    }
    c->redirs[c->nredirs++] = r;
}

static void command_free(command *c) {
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
    free(c->argv);
    for (int i = 0; i < c->nredirs; i++) free(c->redirs[i].target);
    free(c->redirs);
    memset(c, 0, sizeof *c);
}

static void pl_add_command(pipeline *pl, command *c) {
    if (pl->ncmds == pl->cmds_cap) {
        pl->cmds_cap = pl->cmds_cap ? pl->cmds_cap * 2 : 4;
        pl->cmds = xrealloc(pl->cmds, (size_t)pl->cmds_cap * sizeof *pl->cmds);
    }
    pl->cmds[pl->ncmds++] = *c;
    memset(c, 0, sizeof *c);
}

void pipeline_free(pipeline *pl) {
    for (int i = 0; i < pl->ncmds; i++) command_free(&pl->cmds[i]);
    free(pl->cmds);
    free(pl->text);
    memset(pl, 0, sizeof *pl);
}

int parse_line(const char *line, int last_status, pipeline *pl, char *err, size_t errlen) {
    memset(pl, 0, sizeof *pl);
    token_list toks = {0};
    command cur = {0};
    int cur_has_redirect = 0;

    if (tokenize(line, last_status, &toks, err, errlen) < 0) goto fail;

    for (int i = 0; i < toks.count; i++) {
        token *t = &toks.items[i];
        switch (t->type) {
        case T_WORD:
            cmd_add_arg(&cur, t->text);
            t->text = NULL;
            break;
        case T_PIPE:
            if (cur.argc == 0) { snprintf(err, errlen, "syntax error near unexpected token `|'"); goto fail; }
            pl_add_command(pl, &cur);
            cur_has_redirect = 0;
            break;
        case T_AMP:
            if (i + 1 != toks.count || cur.argc == 0) {
                snprintf(err, errlen, "syntax error near unexpected token `&'");
                goto fail;
            }
            pl->background = 1;
            break;
        case T_DUP:
            cmd_add_redirect(&cur, (redirect){R_DUP, t->fd, t->src_fd, NULL});
            cur_has_redirect = 1;
            break;
        default: {   /* <  >  >>  need a file name */
            if (i + 1 >= toks.count) {
                snprintf(err, errlen, "syntax error near unexpected token `newline'");
                goto fail;
            }
            if (toks.items[i + 1].type != T_WORD) {
                snprintf(err, errlen, "syntax error near unexpected token `%s'", tok_symbol(toks.items[i + 1].type));
                goto fail;
            }
            redirect r = {t->type == T_IN ? R_READ : t->type == T_APPEND ? R_APPEND : R_WRITE,
                          t->fd, -1, toks.items[i + 1].text};
            toks.items[i + 1].text = NULL;
            cmd_add_redirect(&cur, r);
            cur_has_redirect = 1;
            i++;
            break;
        }
        }
    }

    if (cur.argc > 0) {
        pl_add_command(pl, &cur);
    } else if (pl->ncmds > 0) {
        snprintf(err, errlen, "syntax error: expected a command after `|'");
        goto fail;
    } else if (cur_has_redirect) {
        snprintf(err, errlen, "syntax error: redirection without a command");
        goto fail;
    }

    /* Keep a trimmed copy of the text (without a trailing &) for job listings. */
    const char *b = line;
    while (*b && isspace((unsigned char)*b)) b++;
    size_t len = strlen(b);
    while (len > 0 && isspace((unsigned char)b[len - 1])) len--;
    if (pl->background && len > 0 && b[len - 1] == '&') {
        len--;
        while (len > 0 && isspace((unsigned char)b[len - 1])) len--;
    }
    pl->text = xstrndup(b, len);

    command_free(&cur);
    tl_free(&toks);
    return 0;

fail:
    command_free(&cur);
    pipeline_free(pl);
    tl_free(&toks);
    return -1;
}
