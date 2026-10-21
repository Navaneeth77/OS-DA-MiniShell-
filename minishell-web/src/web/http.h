/* http.h - a deliberately small HTTP/1.1 server (one request per connection). */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "sbuf.h"

#define MAX_REQUEST_SIZE (1024 * 1024)

typedef struct {
    char method[8];
    char path[512];
    char query[512];
    const char *body;
    size_t body_len;
} http_request;

typedef struct {
    int status;
    const char *content_type;
    sbuf body;
} http_response;

/* Creates a non-blocking listening socket bound to host:port. -1 on error. */
int http_listen(const char *host, int port);

/* 1 = complete request parsed, 0 = need more bytes, -1 = malformed. */
int http_parse(const char *data, size_t len, http_request *req);

/* Writes status line, headers and body; blocks briefly if the socket is full. */
void http_send(int fd, const http_response *res);

/* Returns the integer value of `key` in a query string, or def. */
int query_int(const char *query, const char *key, int def);

#endif
