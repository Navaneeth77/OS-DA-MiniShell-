/* http.c - sockets and HTTP/1.1 request parsing. */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

int http_listen(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    fcntl(fd, F_SETFD, FD_CLOEXEC);   /* children must not inherit the listener */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad address %s\n", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 64) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

int http_parse(const char *data, size_t len, http_request *req) {
    memset(req, 0, sizeof *req);

    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) { end = data + i; break; }
    if (!end) return len > 16384 ? -1 : 0;   /* headers not complete yet */

    /* Request line: METHOD SP TARGET SP VERSION */
    const char *line_end = memchr(data, '\r', (size_t)(end - data) + 1);
    const char *sp1 = memchr(data, ' ', (size_t)(line_end - data));
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2) return -1;

    size_t mlen = (size_t)(sp1 - data);
    if (mlen == 0 || mlen >= sizeof req->method) return -1;
    memcpy(req->method, data, mlen);

    const char *target = sp1 + 1;
    size_t tlen = (size_t)(sp2 - target);
    const char *qmark = memchr(target, '?', tlen);
    size_t plen = qmark ? (size_t)(qmark - target) : tlen;
    if (plen == 0 || plen >= sizeof req->path) return -1;
    memcpy(req->path, target, plen);
    if (qmark) {
        size_t qlen = tlen - plen - 1;
        if (qlen >= sizeof req->query) qlen = sizeof req->query - 1;
        memcpy(req->query, qmark + 1, qlen);
    }

    /* Headers: we only care about Content-Length. */
    size_t content_length = 0;
    for (const char *h = line_end + 2; h < end;) {
        const char *eol = memchr(h, '\r', (size_t)(end - h) + 1);
        if (!eol) break;
        if ((size_t)(eol - h) > 15 && strncasecmp(h, "Content-Length:", 15) == 0)
            content_length = strtoul(h + 15, NULL, 10);
        h = eol + 2;
    }
    if (content_length > MAX_REQUEST_SIZE) return -1;

    size_t header_len = (size_t)(end - data) + 4;
    if (len < header_len + content_length) return 0;   /* body still arriving */
    req->body = data + header_len;
    req->body_len = content_length;
    return 1;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
    }
}

static void write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 2000) <= 0) return;   /* client stopped reading */
        } else {
            return;
        }
    }
}

void http_send(int fd, const http_response *res) {
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     res->status, reason(res->status), res->content_type, res->body.len);
    write_all(fd, head, (size_t)n);
    write_all(fd, res->body.data, res->body.len);
}

int query_int(const char *query, const char *key, int def) {
    size_t klen = strlen(key);
    for (const char *p = query; p && *p;) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') return atoi(p + klen + 1);
        p = strchr(p, '&');
        if (p) p++;
    }
    return def;
}
