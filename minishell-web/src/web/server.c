/* server.c - MiniShell++ web terminal.
 *
 * Runs ./minishell inside a pseudo-terminal (the same kernel device a real
 * terminal emulator uses) and connects it to the browser:
 *
 *   browser keystrokes --POST /api/input--> write(master)  --> tty line discipline --> shell/programs
 *   shell/program output --> read(master) --> Server-Sent Events --> browser terminal
 *
 * Because the shell sees a real TTY, Ctrl-C / Ctrl-Z / Ctrl-D, echo, job
 * control and line-buffered printf/scanf behave exactly as in a Linux terminal.
 *
 * gedit: the shell's gedit program prints a private escape sequence
 * (ESC ] 7777;gedit;PID;HEXPATH BEL). This server records it so the browser
 * may read/save that file, and closing the editor window sends SIGUSR1 to PID.
 *
 * Usage: ./minishell-web [port]      (listens on 127.0.0.1 only)
 */
#include "http.h"
#include "sbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS 64
#define MAX_STREAMS 16
#define MAX_EDITORS 32
#define SCREEN_KEEP (1024 * 1024)      /* replay buffer for page reloads */
#define MAX_FILE (2 * 1024 * 1024)

typedef struct { int fd; sbuf buf; double opened; } connection;
typedef struct { pid_t pid; char path[PATH_MAX]; } editor;

static char g_dir[PATH_MAX];           /* directory holding the binaries and public/ */
static int g_master = -1;              /* pseudo-terminal master */
static pid_t g_shell = -1;
static int g_shell_exited;
static unsigned short g_cols = 100, g_rows = 30;

static sbuf g_screen;                  /* terminal output kept for replay */
static size_t g_base;                  /* absolute offset of g_screen.data[0] */
static size_t g_scan;                  /* absolute offset where escape scanning resumes */

static connection g_conns[MAX_CONNS];
static int g_nconns;
static int g_streams[MAX_STREAMS];
static int g_nstreams;
static editor g_editors[MAX_EDITORS];
static int g_neditors;
static volatile sig_atomic_t g_stop;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------- output */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(sbuf *b, const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        sb_putc(b, B64[(v >> 18) & 63]);
        sb_putc(b, B64[(v >> 12) & 63]);
        sb_putc(b, i + 1 < n ? B64[(v >> 6) & 63] : '=');
        sb_putc(b, i + 2 < n ? B64[v & 63] : '=');
    }
}

static int write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n > 0) { data += n; len -= (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 1000) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void drop_stream(int i) {
    close(g_streams[i]);
    g_streams[i] = g_streams[--g_nstreams];
}

static int send_event(int fd, const char *data, size_t len) {
    sbuf ev;
    sb_init(&ev);
    sb_puts(&ev, "data: ");
    base64(&ev, (const unsigned char *)data, len);
    sb_puts(&ev, "\n\n");
    int r = write_all(fd, ev.data, ev.len);
    sb_free(&ev);
    return r;
}

static void broadcast(const char *data, size_t len) {
    for (int i = g_nstreams - 1; i >= 0; i--)
        if (send_event(g_streams[i], data, len) < 0) drop_stream(i);
}

static void register_editor(pid_t pid, const char *path) {
    for (int i = 0; i < g_neditors; i++)
        if (g_editors[i].pid == pid) return;
    if (g_neditors == MAX_EDITORS) memmove(g_editors, g_editors + 1, (MAX_EDITORS - 1) * sizeof *g_editors), g_neditors--;
    g_editors[g_neditors].pid = pid;
    snprintf(g_editors[g_neditors].path, PATH_MAX, "%s", path);
    g_neditors++;
}

/* Looks for ESC ] 7777;gedit;PID;HEXPATH BEL in new output. */
static void scan_for_editors(void) {
    static const char tag[] = "\033]7777;gedit;";
    const size_t tl = sizeof tag - 1;
    size_t i = g_scan > g_base ? g_scan - g_base : 0;
    const char *d = g_screen.data;
    size_t len = g_screen.len;

    while (i < len) {
        const char *esc = memchr(d + i, '\033', len - i);
        if (!esc) { i = len; break; }
        size_t k = (size_t)(esc - d);
        size_t avail = len - k < tl ? len - k : tl;
        if (memcmp(d + k, tag, avail) != 0) { i = k + 1; continue; }
        if (avail < tl) { i = k; break; }                       /* wait for more bytes */
        const char *bel = memchr(d + k + tl, '\007', len - k - tl);
        if (!bel) { i = k; break; }

        char body[PATH_MAX * 2 + 32];
        size_t blen = (size_t)(bel - (d + k + tl));
        if (blen < sizeof body) {
            memcpy(body, d + k + tl, blen);
            body[blen] = '\0';
            char *semi = strchr(body, ';');
            if (semi) {
                *semi = '\0';
                char path[PATH_MAX];
                size_t n = 0;
                for (char *h = semi + 1; h[0] && h[1] && n < sizeof path - 1; h += 2) {
                    unsigned v;
                    if (sscanf(h, "%2x", &v) != 1) break;
                    path[n++] = (char)v;
                }
                path[n] = '\0';
                register_editor((pid_t)atoi(body), path);
            }
        }
        i = (size_t)(bel - d) + 1;
    }
    g_scan = g_base + i;
}

static void screen_append(const char *data, size_t len) {
    sb_append(&g_screen, data, len);
    scan_for_editors();
    if (g_screen.len > 2 * SCREEN_KEEP) {
        size_t drop = g_screen.len - SCREEN_KEEP;
        memmove(g_screen.data, g_screen.data + drop, g_screen.len - drop + 1);
        g_screen.len -= drop;
        g_base += drop;
    }
    broadcast(data, len);
}

/* ---------------------------------------------------------- the shell */

static void set_winsize(void) {
    struct winsize ws = {g_rows, g_cols, 0, 0};
    if (g_master >= 0) ioctl(g_master, TIOCSWINSZ, &ws);
}

static int spawn_shell(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        perror("posix_openpt");
        return -1;
    }
    char slave_name[256];
    snprintf(slave_name, sizeof slave_name, "%s", ptsname(master));
    g_master = master;
    set_winsize();

    char shell_path[PATH_MAX + 16];
    snprintf(shell_path, sizeof shell_path, "%s/minishell", g_dir);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        /* New session whose controlling terminal is the pty slave. */
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(1);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        for (int s = 1; s < 32; s++) sigaction(s, &sa, NULL);

        setenv("TERM", "xterm-256color", 1);
        setenv("MINISHELL_WEB", "1", 1);
        const char *home = getenv("HOME");
        if (home) { if (chdir(home) < 0) { /* stay where we are */ } }
        execl(shell_path, "minishell", (char *)NULL);
        fprintf(stderr, "cannot start %s: %s\n", shell_path, strerror(errno));
        _exit(127);
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    g_shell = pid;
    g_shell_exited = 0;
    return 0;
}

static void shell_finished(void) {
    if (g_master >= 0) { close(g_master); g_master = -1; }
    int st;
    if (g_shell > 0) waitpid(g_shell, &st, 0);
    g_shell = -1;
    g_shell_exited = 1;
    const char *msg = "\r\n\033[2m[process completed - press Enter to start a new session]\033[0m\r\n";
    screen_append(msg, strlen(msg));
}

static void read_pty(void) {
    char buf[65536];
    for (;;) {
        ssize_t n = read(g_master, buf, sizeof buf);
        if (n > 0) { screen_append(buf, (size_t)n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        shell_finished();   /* EIO: every slave descriptor closed -> the shell exited */
        return;
    }
}

/* ------------------------------------------------------------ routes */

static editor *find_editor(int pid) {
    for (int i = 0; i < g_neditors; i++)
        if (g_editors[i].pid == pid) return &g_editors[i];
    return NULL;
}

/* 'R' running, 'T' stopped, 0 gone */
static char process_state(pid_t pid) {
    if (kill(pid, 0) < 0 && errno == ESRCH) return 0;
    char cmd[64], stat[16] = "";
    snprintf(cmd, sizeof cmd, "ps -o stat= -p %d", (int)pid);
    FILE *p = popen(cmd, "r");
    if (!p) return 'R';
    if (!fgets(stat, sizeof stat, p)) stat[0] = '\0';
    pclose(p);
    char *s = stat;
    while (*s == ' ') s++;
    if (!*s || *s == 'Z') return 0;
    return *s == 'T' ? 'T' : 'R';
}

static void json_err(http_response *res, int status, const char *msg) {
    res->status = status;
    sb_puts(&res->body, "{\"error\":");
    sb_json_cstr(&res->body, msg);
    sb_putc(&res->body, '}');
}

static void handle_editor(const http_request *req, http_response *res, const char *action) {
    int pid = query_int(req->query, "pid", -1);
    editor *e = find_editor(pid);
    if (!e) { json_err(res, 404, "no such editor"); return; }

    if (!strcmp(action, "open")) {
        const char *name = strrchr(e->path, '/');
        name = name ? name + 1 : e->path;
        sb_puts(&res->body, "{\"path\":");
        sb_json_cstr(&res->body, e->path);
        sb_puts(&res->body, ",\"name\":");
        sb_json_cstr(&res->body, name);
        struct stat st;
        int fd = open(e->path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= MAX_FILE) {
            sbuf content;
            sb_init(&content);
            char buf[65536];
            ssize_t n;
            while ((n = read(fd, buf, sizeof buf)) > 0) sb_append(&content, buf, (size_t)n);
            sb_puts(&res->body, ",\"exists\":true,\"content\":");
            sb_json_str(&res->body, content.data, content.len);
            sb_free(&content);
        } else {
            sb_puts(&res->body, ",\"exists\":false,\"content\":\"\"");
        }
        if (fd >= 0) close(fd);
        sb_putc(&res->body, '}');
    } else if (!strcmp(action, "status")) {
        char s = process_state(e->pid);
        sb_printf(&res->body, "{\"alive\":%s,\"stopped\":%s}", s ? "true" : "false", s == 'T' ? "true" : "false");
    } else if (!strcmp(action, "save")) {
        int fd = open(e->path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || write_all(fd, req->body, req->body_len) < 0) {
            if (fd >= 0) close(fd);
            json_err(res, 500, strerror(errno));
            return;
        }
        close(fd);
        sb_printf(&res->body, "{\"saved\":%zu}", req->body_len);
    } else if (!strcmp(action, "close") || !strcmp(action, "kill")) {
        kill(e->pid, !strcmp(action, "close") ? SIGUSR1 : SIGKILL);
        sb_puts(&res->body, "{\"ok\":true}");
    } else {
        json_err(res, 404, "unknown action");
    }
}

static void handle_static(const http_request *req, http_response *res) {
    const char *path = !strcmp(req->path, "/") ? "/index.html" : req->path;
    if (strstr(path, "..")) { json_err(res, 400, "bad path"); return; }
    char full[PATH_MAX * 2];
    snprintf(full, sizeof full, "%s/public%s", g_dir, path);
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        res->status = 404;
        res->content_type = "text/plain";
        sb_puts(&res->body, "not found\n");
        return;
    }
    const char *ext = strrchr(path, '.');
    res->content_type = !ext ? "application/octet-stream"
                      : !strcmp(ext, ".html") ? "text/html; charset=utf-8"
                      : !strcmp(ext, ".css") ? "text/css; charset=utf-8"
                      : !strcmp(ext, ".js") ? "application/javascript; charset=utf-8"
                      : !strcmp(ext, ".svg") ? "image/svg+xml" : "application/octet-stream";
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) sb_append(&res->body, buf, (size_t)n);
    close(fd);
}

/* Returns 1 if the connection was turned into an event stream (kept open). */
static int handle_request(int fd, const http_request *req) {
    http_response res;
    memset(&res, 0, sizeof res);
    sb_init(&res.body);
    res.status = 200;
    res.content_type = "application/json";
    int post = !strcmp(req->method, "POST");
    char action[16];

    if (!strcmp(req->path, "/api/stream")) {
        if (g_nstreams == MAX_STREAMS) drop_stream(0);
        const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                           "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
        write_all(fd, head, strlen(head));
        /* Replay what is still buffered so a reloaded page shows the same screen. */
        char hello[96];
        int n = snprintf(hello, sizeof hello, "event: reset\ndata: %zu\n\n", g_base);
        write_all(fd, hello, (size_t)n);
        size_t chunk = 32768;
        for (size_t off = 0; off < g_screen.len; off += chunk)
            send_event(fd, g_screen.data + off, g_screen.len - off < chunk ? g_screen.len - off : chunk);
        g_streams[g_nstreams++] = fd;
        sb_free(&res.body);
        return 1;
    } else if (!strcmp(req->path, "/api/input") && post) {
        if (g_shell_exited) {
            if (memchr(req->body, '\r', req->body_len) || memchr(req->body, '\n', req->body_len)) {
                const char *reset = "\033[H\033[2J\033[3J";
                screen_append(reset, strlen(reset));
                spawn_shell();
            }
        } else if (g_master >= 0) {
            write_all(g_master, req->body, req->body_len);
        }
        sb_puts(&res.body, "{\"ok\":true}");
    } else if (!strcmp(req->path, "/api/resize") && post) {
        int cols = query_int(req->query, "cols", 0), rows = query_int(req->query, "rows", 0);
        if (cols >= 20 && cols <= 500 && rows >= 5 && rows <= 200) {
            g_cols = (unsigned short)cols;
            g_rows = (unsigned short)rows;
            set_winsize();   /* the kernel sends SIGWINCH to the foreground job */
        }
        sb_printf(&res.body, "{\"cols\":%d,\"rows\":%d}", g_cols, g_rows);
    } else if (sscanf(req->path, "/api/gedit/%15[a-z]", action) == 1) {
        handle_editor(req, &res, action);
    } else if (!strncmp(req->path, "/api/", 5)) {
        json_err(&res, 404, "no such endpoint");
    } else {
        handle_static(req, &res);
    }
    http_send(fd, &res);
    sb_free(&res.body);
    return 0;
}

/* ------------------------------------------------------------- loop */

static void close_conn(int i) {
    sb_free(&g_conns[i].buf);
    g_conns[i] = g_conns[--g_nconns];
}

static void on_stop(int sig) { (void)sig; g_stop = 1; }
static void on_child(int sig) { (void)sig; }

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    if (port <= 0 || port > 65535) { fprintf(stderr, "usage: %s [port]\n", argv[0]); return 2; }

    char exe[PATH_MAX];
    if (strchr(argv[0], '/') && realpath(argv[0], exe)) {
        *strrchr(exe, '/') = '\0';
        snprintf(g_dir, sizeof g_dir, "%s", exe);
    } else if (!getcwd(g_dir, sizeof g_dir)) {
        snprintf(g_dir, sizeof g_dir, ".");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_child;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    sb_init(&g_screen);
    int listen_fd = http_listen("127.0.0.1", port);
    if (listen_fd < 0) return 1;
    if (spawn_shell() < 0) return 1;

    printf("MiniShell++ web terminal\n  open  http://127.0.0.1:%d\n  stop  Ctrl-C\n", port);
    fflush(stdout);

    struct pollfd pfds[2 + MAX_CONNS + MAX_STREAMS];
    while (!g_stop) {
        int n = 0;
        pfds[n++] = (struct pollfd){listen_fd, POLLIN, 0};
        pfds[n++] = (struct pollfd){g_master, POLLIN, 0};   /* fd -1 is ignored by poll */
        int conn_base = n;
        for (int i = 0; i < g_nconns; i++) pfds[n++] = (struct pollfd){g_conns[i].fd, POLLIN, 0};
        int stream_base = n;
        for (int i = 0; i < g_nstreams; i++) pfds[n++] = (struct pollfd){g_streams[i], POLLIN, 0};

        int ready = poll(pfds, (nfds_t)n, 500);
        if (ready < 0 && errno != EINTR) { perror("poll"); break; }

        if (g_master >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) read_pty();

        /* A closed browser tab shows up as readable/hung-up on its stream socket. */
        for (int i = g_nstreams - 1; i >= 0; i--) {
            if (stream_base + i >= n || !pfds[stream_base + i].revents) continue;
            char tmp[256];
            if (read(g_streams[i], tmp, sizeof tmp) <= 0) drop_stream(i);
        }

        for (int i = g_nconns - 1; i >= 0; i--) {
            if (!pfds[conn_base + i].revents) continue;
            connection *c = &g_conns[i];
            char buf[16384];
            ssize_t r;
            int closed = 0;
            while ((r = read(c->fd, buf, sizeof buf)) > 0) sb_append(&c->buf, buf, (size_t)r);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) closed = 1;

            http_request req;
            int parsed = http_parse(c->buf.data, c->buf.len, &req);
            if (parsed == 1) {
                int fd = c->fd;
                int kept = handle_request(fd, &req);
                close_conn(i);
                if (!kept) close(fd);
            } else if (parsed < 0 || closed) {
                close(c->fd);
                close_conn(i);
            }
        }
        double now = now_ms();
        for (int i = g_nconns - 1; i >= 0; i--)
            if (now - g_conns[i].opened > 10000) { close(g_conns[i].fd); close_conn(i); }

        if (pfds[0].revents & POLLIN) {
            int fd;
            while ((fd = accept(listen_fd, NULL, NULL)) >= 0) {
                if (g_nconns == MAX_CONNS) { close(fd); continue; }
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                g_conns[g_nconns].fd = fd;
                sb_init(&g_conns[g_nconns].buf);
                g_conns[g_nconns].opened = now;
                g_nconns++;
            }
        }

        /* Reap the shell if it exited without us noticing EIO yet. */
        if (g_shell > 0) {
            int st;
            if (waitpid(g_shell, &st, WNOHANG) == g_shell) {
                g_shell = 0;
                if (g_master >= 0) read_pty();
                if (!g_shell_exited) shell_finished();
            }
        }
    }

    printf("\nstopping\n");
    if (g_shell > 0) kill(g_shell, SIGHUP);
    if (g_master >= 0) close(g_master);
    for (int i = 0; i < g_nstreams; i++) close(g_streams[i]);
    close(listen_fd);
    return 0;
}
