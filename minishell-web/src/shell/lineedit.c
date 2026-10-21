/* lineedit.c - reads a command line in raw terminal mode so the arrow keys
 * work: ←/→ move, ↑/↓ history, Backspace, Delete, Home/End, Ctrl-A/E/U/K/W/L,
 * Ctrl-C cancels the line, Ctrl-D on an empty line is EOF. */
#include "sbuf.h"
#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    sbuf buf;
    size_t pos;   /* cursor position in bytes */
} line_state;

static void out(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void outs(const char *s) { out(s, strlen(s)); }

static void move_left(size_t n) {
    if (!n) return;
    char seq[32];
    snprintf(seq, sizeof seq, "\033[%zuD", n);
    outs(seq);
}

/* Redraws the edited text; the terminal cursor is currently at old_pos. */
static void refresh(line_state *ls, size_t old_pos) {
    move_left(old_pos);
    out(ls->buf.data, ls->buf.len);
    outs("\033[K");
    move_left(ls->buf.len - ls->pos);
}

static void set_text(line_state *ls, const char *text) {
    size_t old = ls->pos;
    sb_clear(&ls->buf);
    sb_puts(&ls->buf, text);
    ls->pos = ls->buf.len;
    refresh(ls, old);
}

static void delete_range(line_state *ls, size_t from, size_t to) {
    memmove(ls->buf.data + from, ls->buf.data + to, ls->buf.len - to + 1);
    ls->buf.len -= to - from;
}

/* Non-interactive fallback: plain buffered line reading. */
static int read_plain(char **line) {
    static sbuf pending;
    static int init;
    if (!init) { sb_init(&pending); init = 1; }
    for (;;) {
        char *nl = memchr(pending.data, '\n', pending.len);
        if (nl) {
            size_t n = (size_t)(nl - pending.data);
            *line = xstrndup(pending.data, n);
            memmove(pending.data, nl + 1, pending.len - n);
            pending.len -= n + 1;
            return 1;
        }
        char chunk[4096];
        ssize_t r = read(STDIN_FILENO, chunk, sizeof chunk);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (!pending.len) return 0;
            *line = xstrndup(pending.data, pending.len);
            sb_clear(&pending);
            return 1;
        }
        sb_append(&pending, chunk, (size_t)r);
    }
}

int read_command(const char *prompt, char **line) {
    if (!sh.interactive) return read_plain(line);

    struct termios raw = sh.tmodes;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN);   /* keep ISIG: Ctrl-C still raises SIGINT */
    raw.c_iflag &= ~(tcflag_t)(IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(sh.tty, TCSADRAIN, &raw);

    outs(prompt);

    line_state ls;
    sb_init(&ls.buf);
    ls.pos = 0;
    strlist *h = &sh.history;
    int hist = h->count;        /* h->count means "the line being typed" */
    char *draft = NULL;
    int result;

    for (;;) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_sighup) { result = 0; break; }
                if (g_sigint) {
                    g_sigint = 0;
                    outs("^C\r\n");
                    result = -1;
                    break;
                }
                continue;
            }
            result = 0;
            break;
        }
        if (n == 0) { result = 0; break; }

        size_t old = ls.pos;
        switch (c) {
        case '\r':
        case '\n':
            outs("\r\n");
            result = 1;
            goto done;
        case 4:   /* Ctrl-D */
            if (ls.buf.len == 0) { result = 0; goto done; }
            if (ls.pos < ls.buf.len) { delete_range(&ls, ls.pos, ls.pos + 1); refresh(&ls, old); }
            break;
        case 127:
        case 8:   /* Backspace */
            if (ls.pos > 0) {
                delete_range(&ls, ls.pos - 1, ls.pos);
                ls.pos--;
                refresh(&ls, old);
            }
            break;
        case 1: ls.pos = 0; refresh(&ls, old); break;                 /* Ctrl-A */
        case 5: ls.pos = ls.buf.len; refresh(&ls, old); break;        /* Ctrl-E */
        case 2: if (ls.pos) { ls.pos--; outs("\033[D"); } break;      /* Ctrl-B */
        case 6: if (ls.pos < ls.buf.len) { ls.pos++; outs("\033[C"); } break;
        case 21: delete_range(&ls, 0, ls.pos); ls.pos = 0; refresh(&ls, old); break;          /* Ctrl-U */
        case 11: ls.buf.len = ls.pos; ls.buf.data[ls.pos] = '\0'; refresh(&ls, old); break;  /* Ctrl-K */
        case 23: {  /* Ctrl-W */
            size_t p = ls.pos;
            while (p > 0 && ls.buf.data[p - 1] == ' ') p--;
            while (p > 0 && ls.buf.data[p - 1] != ' ') p--;
            delete_range(&ls, p, ls.pos);
            ls.pos = p;
            refresh(&ls, old);
            break;
        }
        case 12:  /* Ctrl-L */
            outs("\033[H\033[2J");
            outs(prompt);
            out(ls.buf.data, ls.buf.len);
            move_left(ls.buf.len - ls.pos);
            break;
        case 27: {  /* escape sequences: arrows, Home, End, Delete */
            unsigned char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (seq[0] != '[' && seq[0] != 'O') break;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) break;
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) break;
                if (seq[2] != '~') break;
                if (seq[1] == '3' && ls.pos < ls.buf.len) { delete_range(&ls, ls.pos, ls.pos + 1); refresh(&ls, old); }
                if (seq[1] == '1' || seq[1] == '7') { ls.pos = 0; refresh(&ls, old); }
                if (seq[1] == '4' || seq[1] == '8') { ls.pos = ls.buf.len; refresh(&ls, old); }
                break;
            }
            switch (seq[1]) {
            case 'A':   /* up: older history */
                if (hist > 0) {
                    if (hist == h->count) { free(draft); draft = xstrdup(ls.buf.data); }
                    hist--;
                    set_text(&ls, h->items[hist]);
                }
                break;
            case 'B':   /* down: newer history */
                if (hist < h->count) {
                    hist++;
                    set_text(&ls, hist == h->count ? (draft ? draft : "") : h->items[hist]);
                }
                break;
            case 'C': if (ls.pos < ls.buf.len) { ls.pos++; outs("\033[C"); } break;
            case 'D': if (ls.pos > 0) { ls.pos--; outs("\033[D"); } break;
            case 'H': ls.pos = 0; refresh(&ls, old); break;
            case 'F': ls.pos = ls.buf.len; refresh(&ls, old); break;
            }
            break;
        }
        case '\t':
            break;   /* no completion */
        default:
            if (c < 32) break;
            if (ls.pos == ls.buf.len) {
                sb_putc(&ls.buf, (char)c);
                ls.pos++;
                out((const char *)&c, 1);
            } else {
                sb_putc(&ls.buf, ' ');
                memmove(ls.buf.data + ls.pos + 1, ls.buf.data + ls.pos, ls.buf.len - ls.pos - 1);
                ls.buf.data[ls.pos] = (char)c;
                ls.pos++;
                refresh(&ls, old);
            }
        }
    }

done:
    tcsetattr(sh.tty, TCSADRAIN, &sh.tmodes);
    free(draft);
    if (result == 1) *line = ls.buf.data;
    else sb_free(&ls.buf);
    return result;
}
