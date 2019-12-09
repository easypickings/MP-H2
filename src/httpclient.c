/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "h2o.h"

#ifndef MIN
#define MIN(a, b) (((a) > (b)) ? (b) : (a))
#endif

static const char *path;
static const char *file;
static int fd;
static int file_size = -1;
static double delta = 0.05;
int new_once = 1;
static download_path_t download_path[3] = {{
                                               1,    /* cnt_left */
                                               0,    /* rtt */
                                               0,    /* bandwidth */
                                               NULL, /* conn */
                                               NULL, /* client */
                                               NULL, /* second_client */
                                               NULL, /* ctx */
                                               NULL, /* connpool */
                                           },
                                           {
                                               0,    /* cnt_left */
                                               0,    /* rtt */
                                               0,    /* bandwidth */
                                               NULL, /* conn */
                                               NULL, /* client */
                                               NULL, /* second_client */
                                               NULL, /* ctx */
                                               NULL, /* connpool */
                                           },
                                           {
                                               0,    /* cnt_left */
                                               0,    /* rtt */
                                               0,    /* bandwidth */
                                               NULL, /* conn */
                                               NULL, /* client */
                                               NULL, /* second_client */
                                               NULL, /* ctx */
                                               NULL, /* connpool */
                                           }};

// static h2o_httpclient_connection_pool_t *connpool;
// static h2o_mem_pool_t pool;
// static const char *url;
static char *method = "GET";
// static int cnt_left = 1;
// static int body_size = 0;
// static int chunk_size = 10;
// static h2o_iovec_t iov_filler;
static int delay_interval_ms = 0;
static int ssl_verify_none = 1;
// static int http2_ratio = 100;
static int cur_body_size = 0;

static h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *method, h2o_url_t *url,
                                         const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                         h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                         h2o_url_t *origin);
static h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, int version, int status, h2o_iovec_t msg,
                                      h2o_header_t *headers, size_t num_headers, int header_requires_dup);

static void on_exit_deferred(h2o_timer_t *entry)
{
    h2o_timer_unlink(entry);
    exit(1);
}
static h2o_timer_t exit_deferred;

static void on_error(h2o_httpclient_ctx_t *ctx, const char *fmt, ...)
{
    char errbuf[2048];
    va_list args;
    va_start(args, fmt);
    int errlen = vsnprintf(errbuf, sizeof(errbuf), fmt, args);
    va_end(args);
    fprintf(stderr, "%.*s\n", errlen, errbuf);

    /* defer using zero timeout to send pending GOAWAY frame */
    memset(&exit_deferred, 0, sizeof(exit_deferred));
    exit_deferred.cb = on_exit_deferred;
    h2o_timer_link(ctx->loop, 0, &exit_deferred);
}

static void start_request(h2o_httpclient_ctx_t *ctx)
{
    h2o_url_t *url_parsed;
    download_path_t *path = (download_path_t *)ctx->path;

    /* parse URL */
    url_parsed = h2o_mem_alloc_pool(&path->pool, *url_parsed, 1);
    if (h2o_url_parse(path->url, SIZE_MAX, url_parsed) != 0) {
        on_error(ctx, "unrecognized type of URL: %s", path->url);
        return;
    }

    // cur_body_size = body_size;

    /* initiate the request */
    if (path->connpool == NULL) {
        path->connpool = h2o_mem_alloc(sizeof(*(path->connpool)));
        h2o_socketpool_t *sockpool = h2o_mem_alloc(sizeof(*sockpool));
        h2o_socketpool_target_t *target = h2o_socketpool_create_target(url_parsed, NULL);
        h2o_socketpool_init_specific(sockpool, 10, &target, 1, NULL);
        h2o_socketpool_set_timeout(sockpool, 5000 /* in msec */);
        h2o_socketpool_register_loop(sockpool, ctx->loop);
        h2o_httpclient_connection_pool_init(path->connpool, sockpool);

        /* obtain root */
        char *root, *crt_fullpath;
        if ((root = getenv("H2O_ROOT")) == NULL)
            root = H2O_TO_STR(H2O_ROOT);
#define CA_PATH "/share/h2o/ca-bundle.crt"
        crt_fullpath = h2o_mem_alloc(strlen(root) + strlen(CA_PATH) + 1);
        sprintf(crt_fullpath, "%s%s", root, CA_PATH);
#undef CA_PATH

        SSL_CTX *ssl_ctx = SSL_CTX_new(TLSv1_2_client_method());
        SSL_CTX_load_verify_locations(ssl_ctx, crt_fullpath, NULL);
        if (ssl_verify_none) {
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
        } else {
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        }
        h2o_socketpool_set_ssl_ctx(sockpool, ssl_ctx);
        SSL_CTX_free(ssl_ctx);
    }
    h2o_httpclient_connect(NULL, &path->pool, url_parsed, ctx, path->connpool, url_parsed, on_connect);
}

static int on_body(h2o_httpclient_t *client, const char *errstr)
{
    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        on_error(client->ctx, errstr);
        return -1;
    }

    download_path_t *path = (download_path_t *)client->path;

    size_t size_to_write = MIN(path->range.bytes_to_download, (*client->buf)->size);
    pwrite(fd, (*client->buf)->bytes, size_to_write, path->range.start + path->range.bytes_downloaded);
    path->range.bytes_downloaded += size_to_write;
    path->range.bytes_to_download -= size_to_write;
    h2o_buffer_consume(&(*client->buf), (*client->buf)->size);

    if (path->range.bytes_to_download == 0) {
        printf("path %p stream %p downloaded %d bytes range=%d-%d\n", path, path->client, path->range.bytes_downloaded,
               path->range.start, path->range.end);
        return -1;
    }

    double rest_time[3], this_rest_time;
    for (int i = 0; i < 3; ++i) {
        rest_time[i] = download_path[i].range.bytes_to_download / download_path[i].bandwidth;
        if (path == &download_path[i])
            this_rest_time = rest_time[i];
    }

    download_path_t *path_to_help = path;
    double delta_time = delta;
    for (int i = 0; i < 3; ++i) {
        if (rest_time[i] - this_rest_time < 0) {
            /* I'm not the fastest, none of my busness */
            path_to_help = path;
            break;
        } else {
            if (rest_time[i] - this_rest_time > delta_time) {
                /* path[i] is slower than me by more than delta_time, help it! */
                delta_time = rest_time[i] - this_rest_time;
                path_to_help = &download_path[i];
            }
        }
    }

    if (path_to_help != path) {
        /* I'm the fastest, try to help another */
        if (this_rest_time <= path->rtt) {
            /* rest time less than a RTT */
            if (path->second_client == NULL) {
                /* there's only one stream on the path */
                size_t this_new_size, other_new_size;
                other_new_size = (path_to_help->range.bytes_to_download + this_rest_time * path->bandwidth) *
                                 path_to_help->bandwidth / (path->bandwidth + path_to_help->bandwidth);
                this_new_size = path_to_help->range.bytes_to_download - other_new_size;

                /* adjust another's range */
                path_to_help->range.end -= (path_to_help->range.bytes_to_download - other_new_size);
                path_to_help->range.bytes_to_download = other_new_size;

                /* open a new stream on current path */
                path->second_range.start = path_to_help->range.end + 1;
                path->second_range.end = path->second_range.start + this_new_size - 1;
                path->second_range.bytes_to_download = this_new_size;
                path->second_range.bytes_downloaded = 0;
                path->second_range.valid = 1;
                ++path->cnt_left;
                printf("adjust path %p stream %p range=%d-%d\n", path_to_help, path_to_help->client, path_to_help->range.start,
                       path_to_help->range.end);
                printf("new stream on path %p range=%d-%d\n", path, path->second_range.start, path->second_range.end);
                start_request(path->ctx);
            }
        }
    }

    if (errstr == h2o_httpclient_error_is_eos) {
        if (--path->cnt_left != 0) {
            /* next attempt */
            h2o_mem_clear_pool(&path->pool);
            start_request(client->ctx);
        }
    }

    return 0;
}

static void print_status_line(int version, int status, h2o_iovec_t msg)
{
    printf("HTTP/%d", (version >> 8));
    if ((version & 0xff) != 0) {
        printf(".%d", version & 0xff);
    }
    printf(" %d", status);
    if (msg.len != 0) {
        printf(" %.*s\n", (int)msg.len, msg.base);
    } else {
        printf("\n");
    }
}

h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, int version, int status, h2o_iovec_t msg,
                               h2o_header_t *headers, size_t num_headers, int header_requires_dup)
{
    size_t i;

    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        on_error(client->ctx, errstr);
        return NULL;
    }

    print_status_line(version, status, msg);

    download_path_t *path = (download_path_t *)client->path;

    for (i = 0; i != num_headers; ++i) {
        const char *name = headers[i].orig_name;
        if (name == NULL)
            name = headers[i].name->base;
        printf("%.*s: %.*s\n", (int)headers[i].name->len, name, (int)headers[i].value.len, headers[i].value.base);
        if (file_size < 0 && path == &download_path[0]) {
            if (strcmp(name, "content-length") == 0) {
                file_size = atoi(headers[i].value.base);
                unsigned int chunk_size = file_size / 3;

                download_path[0].range.start = 0;
                download_path[0].range.end = chunk_size - 1;
                download_path[0].range.valid = 1;
                download_path[0].range.bytes_to_download = chunk_size;

                download_path[1].range.start = chunk_size;
                download_path[1].range.end = 2 * chunk_size - 1;
                download_path[1].range.valid = 1;
                download_path[1].range.bytes_to_download = chunk_size;
                download_path[1].cnt_left = 1;
                start_request(download_path[1].ctx);

                download_path[2].range.start = 2 * chunk_size;
                download_path[2].range.end = file_size - 1;
                download_path[2].range.valid = 1;
                download_path[2].range.bytes_to_download = file_size - 2 * chunk_size;
                download_path[2].cnt_left = 1;
                start_request(download_path[2].ctx);
            }
        }
    }
    printf("path %p stream %p\n", path, path->client);
    printf("\n");

    if (errstr == h2o_httpclient_error_is_eos) {
        on_error(client->ctx, "no body");
        return NULL;
    }

    client->timings.response_start_at = h2o_gettimeofday(client->ctx->loop);

    return on_body;
}

int fill_body(h2o_iovec_t *reqbuf)
{
    // if (cur_body_size > 0) {
    //     memcpy(reqbuf, &iov_filler, sizeof(*reqbuf));
    //     reqbuf->len = MIN(iov_filler.len, cur_body_size);
    //     cur_body_size -= reqbuf->len;
    //     return 0;
    // } else {
    //     *reqbuf = h2o_iovec_init(NULL, 0);
    //     return 1;
    // }
    *reqbuf = h2o_iovec_init(NULL, 0);
    return 1;
}

struct st_timeout_ctx {
    h2o_httpclient_t *client;
    h2o_timer_t _timeout;
};
static void timeout_cb(h2o_timer_t *entry)
{
    static h2o_iovec_t reqbuf;
    struct st_timeout_ctx *tctx = H2O_STRUCT_FROM_MEMBER(struct st_timeout_ctx, _timeout, entry);

    fill_body(&reqbuf);
    h2o_timer_unlink(&tctx->_timeout);
    tctx->client->write_req(tctx->client, reqbuf, cur_body_size <= 0);
    free(tctx);

    return;
}

static void proceed_request(h2o_httpclient_t *client, size_t written, int is_end_stream)
{
    if (cur_body_size > 0) {
        struct st_timeout_ctx *tctx;
        tctx = h2o_mem_alloc(sizeof(*tctx));
        memset(tctx, 0, sizeof(*tctx));
        tctx->client = client;
        tctx->_timeout.cb = timeout_cb;
        h2o_timer_link(client->ctx->loop, delay_interval_ms, &tctx->_timeout);
    }
}

h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *_method, h2o_url_t *url,
                                  const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                  h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                  h2o_url_t *origin)
{
    if (errstr != NULL) {
        on_error(client->ctx, errstr);
        return NULL;
    }

    *_method = h2o_iovec_init(method, strlen(method));
    *url = *((h2o_url_t *)client->data);
    *headers = NULL;
    *num_headers = 0;
    *body = h2o_iovec_init(NULL, 0);
    *proceed_req_cb = NULL;

    h2o_headers_t headers_vec = (h2o_headers_t){NULL};
    download_path_t *path = (download_path_t *)client->path;
    if (path->range.valid) {
        char msg[2048];
        sprintf(msg, "bytes=%d-%d", path->range.start, path->range.end);
        h2o_add_header(&path->pool, &headers_vec, H2O_TOKEN_RANGE, NULL, msg, strlen(msg));
        *headers = headers_vec.entries;
        *num_headers += 1;
    }

    if (cur_body_size > 0) {
        char *clbuf = h2o_mem_alloc_pool(&path->pool, char, sizeof(H2O_UINT32_LONGEST_STR) - 1);
        size_t clbuf_len = sprintf(clbuf, "%d", cur_body_size);
        h2o_add_header(&path->pool, &headers_vec, H2O_TOKEN_CONTENT_LENGTH, NULL, clbuf, clbuf_len);
        *headers = headers_vec.entries;
        *num_headers += 1;

        *proceed_req_cb = proceed_request;

        struct st_timeout_ctx *tctx;
        tctx = h2o_mem_alloc(sizeof(*tctx));
        memset(tctx, 0, sizeof(*tctx));
        tctx->client = client;
        tctx->_timeout.cb = timeout_cb;
        h2o_timer_link(client->ctx->loop, delay_interval_ms, &tctx->_timeout);
    }

    return on_head;
}

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s -t <path> -o <file> <url1> <url2> <url3>\n", progname);
}
int main(int argc, char **argv)
{
    int opt;

    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    while ((opt = getopt(argc, argv, "t:o:")) != -1) {
        switch (opt) {
        case 't':
            path = optarg;
            break;
        case 'o':
            file = optarg;
            break;
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
        }
    }
    if (argc - optind != 3) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    sprintf(download_path[0].url, "https://%s%s", argv[argc - 3], path);
    sprintf(download_path[1].url, "https://%s%s", argv[argc - 2], path);
    sprintf(download_path[2].url, "https://%s%s", argv[argc - 1], path);

    fd = open(file, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
    if (fd < 0) {
        perror("");
        exit(-1);
    }

    h2o_multithread_queue_t *queue;
    h2o_multithread_receiver_t getaddr_receiver;

    const uint64_t timeout = 5000; /* 5 seconds */
    h2o_httpclient_ctx_t ctx[3] = {
        {NULL,                                     /* loop */
         &getaddr_receiver, timeout,               /* io_timeout */
         timeout,                                  /* connect_timeout */
         timeout,                                  /* first_byte_timeout */
         NULL,                                     /* websocket_timeout */
         timeout,                                  /* keepalive_timeout */
         H2O_SOCKET_INITIAL_INPUT_BUFFER_SIZE * 2, /* max_buffer_size */
         (void *)&download_path[0]},               /* path */

        {NULL,                                     /* loop */
         &getaddr_receiver, timeout,               /* io_timeout */
         timeout,                                  /* connect_timeout */
         timeout,                                  /* first_byte_timeout */
         NULL,                                     /* websocket_timeout */
         timeout,                                  /* keepalive_timeout */
         H2O_SOCKET_INITIAL_INPUT_BUFFER_SIZE * 2, /* max_buffer_size */
         (void *)&download_path[1]},               /* path */

        {NULL,                                     /* loop */
         &getaddr_receiver, timeout,               /* io_timeout */
         timeout,                                  /* connect_timeout */
         timeout,                                  /* first_byte_timeout */
         NULL,                                     /* websocket_timeout */
         timeout,                                  /* keepalive_timeout */
         H2O_SOCKET_INITIAL_INPUT_BUFFER_SIZE * 2, /* max_buffer_size */
         (void *)&download_path[2]}                /* path */
    };

    for (int i = 0; i < 3; ++i) {
        ctx[i].http2.ratio = 100;
        download_path[i].ctx = &ctx[i];
        h2o_mem_init_pool(&download_path[i].pool);
        download_path[i].range.start = 0;
        download_path[i].range.end = 0;
        download_path[i].range.bytes_downloaded = 0;
        download_path[i].range.bytes_to_download = 0;
        download_path[i].range.valid = 0;
        download_path[i].second_range.start = 0;
        download_path[i].second_range.end = 0;
        download_path[i].second_range.bytes_downloaded = 0;
        download_path[i].second_range.bytes_to_download = 0;
        download_path[i].second_range.valid = 0;
    }

    // download_path[0].range.start = 0;
    // download_path[0].range.end = 81919;
    // download_path[0].range.valid = 1;
    // download_path[0].range.bytes_downloaded = 0;
    // download_path[0].range.bytes_to_download = 81920;

/* setup context */
#if H2O_USE_LIBUV
    ctx[0].loop = uv_loop_new();
#else
    ctx[0].loop = h2o_evloop_create();
#endif

    queue = h2o_multithread_create_queue(ctx[0].loop);
    h2o_multithread_register_receiver(queue, ctx[0].getaddr_receiver, h2o_hostinfo_getaddr_receiver);

    ctx[2].loop = ctx[1].loop = ctx[0].loop;

    /* setup the first request */
    struct timeval start_download = h2o_gettimeofday(ctx->loop);
    start_request(&ctx[0]);

    while (download_path[0].cnt_left != 0 || download_path[1].cnt_left != 0 || download_path[2].cnt_left != 0) {
#if H2O_USE_LIBUV
        uv_run(ctx[0].loop, UV_RUN_ONCE);
#else
        h2o_evloop_run(ctx[0].loop, INT32_MAX);
#endif
    }
    struct timeval end_download = h2o_gettimeofday(ctx->loop);
    double time = (end_download.tv_sec - start_download.tv_sec) + (end_download.tv_usec - start_download.tv_usec) / (double)1000000;
    printf("DOWNLOAD TAKES %fs.\n", time);

    return 0;
}
