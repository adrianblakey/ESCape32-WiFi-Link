/*
 * linux_httpd.c  —  POSIX HTTP/1.1 + WebSocket server
 *
 * Drop-in replacement for esp_http_server when building natively on Linux/macOS.
 * Implements exactly the subset of the esp_http_server API that main.c uses.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_idf_shims.h"

/* ── SHA-1 (RFC 3174) — needed for WS handshake ─────────────────── */
typedef struct {
    uint32_t h[5];
    uint8_t  buf[64];
    uint64_t total;
    size_t   blen;
} sha1_t;

#define ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void sha1_compress(sha1_t *c) {
    uint32_t w[80], a,b,cc,d,e,t;
    for (int i=0;i<16;i++) {
        w[i] = ((uint32_t)c->buf[i*4+0]<<24)|((uint32_t)c->buf[i*4+1]<<16)
              |((uint32_t)c->buf[i*4+2]<< 8)|((uint32_t)c->buf[i*4+3]);
    }
    for (int i=16;i<80;i++) w[i]=ROL32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=c->h[0];b=c->h[1];cc=c->h[2];d=c->h[3];e=c->h[4];
    for (int i=0;i<80;i++) {
        uint32_t f,k;
        if      (i<20){f=(b&cc)|(~b&d);k=0x5A827999;}
        else if (i<40){f=b^cc^d;       k=0x6ED9EBA1;}
        else if (i<60){f=(b&cc)|(b&d)|(cc&d);k=0x8F1BBCDC;}
        else          {f=b^cc^d;       k=0xCA62C1D6;}
        t=ROL32(a,5)+f+e+k+w[i]; e=d; d=cc; cc=ROL32(b,30); b=a; a=t;
    }
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;
}
static void sha1_init(sha1_t *c){
    c->h[0]=0x67452301;c->h[1]=0xEFCDAB89;c->h[2]=0x98BADCFE;
    c->h[3]=0x10325476;c->h[4]=0xC3D2E1F0;
    c->total=0;c->blen=0;
}
static void sha1_feed(sha1_t *c, const void *data, size_t len) {
    const uint8_t *p=data; c->total+=len;
    while(len--){ c->buf[c->blen++]=*p++; if(c->blen==64){sha1_compress(c);c->blen=0;} }
}
static void sha1_done(sha1_t *c, uint8_t out[20]) {
    uint64_t bits=c->total*8;
    c->buf[c->blen++]=0x80;
    while(c->blen!=56){if(c->blen==64){sha1_compress(c);c->blen=0;}c->buf[c->blen++]=0;}
    for(int i=7;i>=0;i--) c->buf[c->blen++]=(bits>>(i*8))&0xff;
    sha1_compress(c);
    for(int i=0;i<5;i++){out[i*4]=(c->h[i]>>24)&0xff;out[i*4+1]=(c->h[i]>>16)&0xff;
                          out[i*4+2]=(c->h[i]>>8)&0xff;out[i*4+3]=c->h[i]&0xff;}
}

/* ── Base64 encode ───────────────────────────────────────────────── */
static const char B64C[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_enc(const uint8_t *in, size_t n, char *out) {
    size_t i=0,j=0;
    while(i<n){
        uint32_t a=i<n?in[i++]:0, b=i<n?in[i++]:0, d=i<n?in[i++]:0;
        uint32_t t=(a<<16)|(b<<8)|d;
        out[j++]=B64C[(t>>18)&0x3f]; out[j++]=B64C[(t>>12)&0x3f];
        out[j++]=(i>n+1)?'=':B64C[(t>>6)&0x3f];
        out[j++]=(i>n  )?'=':B64C[(t>>0)&0x3f];
    }
    out[j]='\0';
}

/* ── Server state ────────────────────────────────────────────────── */
#define MAX_HANDLERS 16

typedef struct {
    int                 listen_fd;
    int                 port;
    int                 n_uris;
    httpd_uri_t         uris[MAX_HANDLERS];
    httpd_err_handler_t err404;
} server_t;

/* Per-connection context — stored in req._ws_ctx */
typedef struct {
    int    fd;
    char   status[64];
    char   ctype[128];
    struct { char k[128]; char v[128]; } hdrs[16];
    int    n_hdrs;
    /* WS: payload stashed between recv_frame(max=0) and recv_frame(max=N) */
    uint8_t *ws_payload;
    size_t   ws_payload_len;
    int      ws_opcode;
} conn_ctx_t;

/* ── URI matching ───────────────────────────────────────────────── */
bool httpd_uri_match_wildcard(const char *tmpl, const char *uri, size_t ulen) {
    (void)ulen;
    size_t tlen = strlen(tmpl);
    if (!tlen) return false;
    if (tmpl[tlen-1] == '*')
        return strncmp(tmpl, uri, tlen-1) == 0;
    return strcmp(tmpl, uri) == 0;
}

/* ── httpd_start ────────────────────────────────────────────────── */
esp_err_t httpd_start(httpd_handle_t *hdl, const httpd_config_t *cfg) {
    server_t *srv = calloc(1, sizeof *srv);
    srv->port = cfg->server_port ? cfg->server_port : 80;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) { perror("socket"); free(srv); return ESP_FAIL; }

    int opt=1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt);

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(srv->port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv->listen_fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        perror("bind"); free(srv); return ESP_FAIL;
    }
    listen(srv->listen_fd, 16);
    *hdl = srv;
    ESP_LOGI("httpd", "Listening on port %d", srv->port);
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t hdl, const httpd_uri_t *u) {
    server_t *srv = hdl;
    if (srv->n_uris >= MAX_HANDLERS) return ESP_FAIL;
    srv->uris[srv->n_uris++] = *u;
    return ESP_OK;
}

esp_err_t httpd_register_err_handler(httpd_handle_t hdl,
                                      httpd_err_code_t e,
                                      httpd_err_handler_t fn) {
    server_t *srv = hdl;
    if (e == HTTPD_404_NOT_FOUND) srv->err404 = fn;
    return ESP_OK;
}

/* ── Response helpers ───────────────────────────────────────────── */
static conn_ctx_t *ctx(httpd_req_t *r) { return (conn_ctx_t*)r->_ws_ctx; }

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) {
    strncpy(ctx(r)->status, s, sizeof ctx(r)->status - 1); return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) {
    strncpy(ctx(r)->ctype, t, sizeof ctx(r)->ctype - 1); return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) {
    conn_ctx_t *c = ctx(r);
    if (c->n_hdrs < 16) {
        strncpy(c->hdrs[c->n_hdrs].k, k, 127);
        strncpy(c->hdrs[c->n_hdrs].v, v, 127);
        c->n_hdrs++;
    }
    return ESP_OK;
}

static void send_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) { ssize_t w = write(fd, p, n); if (w<=0) break; p+=w; n-=w; }
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *body, ssize_t blen) {
    conn_ctx_t *c = ctx(r);
    if (blen < 0 && body) blen = (ssize_t)strlen(body);
    if (blen < 0) blen = 0;

    char hbuf[2048]; int n=0;
    n += snprintf(hbuf+n, sizeof hbuf-n,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zd\r\nConnection: close\r\n",
        c->status[0] ? c->status : "200 OK",
        c->ctype[0]  ? c->ctype  : "text/plain",
        (size_t)blen);
    for (int i=0; i<c->n_hdrs; i++)
        n += snprintf(hbuf+n, sizeof hbuf-n, "%s: %s\r\n", c->hdrs[i].k, c->hdrs[i].v);
    n += snprintf(hbuf+n, sizeof hbuf-n, "\r\n");

    send_all(c->fd, hbuf, n);
    if (body && blen > 0) send_all(c->fd, body, blen);
    return ESP_OK;
}

/* ── WebSocket I/O ──────────────────────────────────────────────── */
static int recv_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

/* Read one raw WS frame from socket into heap-allocated buffer.
   Returns 0 on success, -1 on error.
   opcode and payload/plen set on success; caller frees payload. */
static int ws_read_frame(int fd, int *opcode, uint8_t **payload, size_t *plen) {
    uint8_t h2[2];
    if (recv_exact(fd, h2, 2) < 0) return -1;
    *opcode       = h2[0] & 0x0f;
    bool masked   = (h2[1] & 0x80) != 0;
    size_t length = h2[1] & 0x7f;
    if (length == 126) {
        uint8_t e[2]; if (recv_exact(fd,e,2)<0) return -1;
        length = ((size_t)e[0]<<8)|e[1];
    } else if (length == 127) {
        uint8_t e[8]; if (recv_exact(fd,e,8)<0) return -1;
        length=0; for(int i=0;i<8;i++) length=(length<<8)|e[i];
    }
    uint8_t mask[4]={0};
    if (masked && recv_exact(fd, mask, 4) < 0) return -1;
    *payload = malloc(length+1);
    if (!*payload) return -1;
    if (length && recv_exact(fd, *payload, length) < 0) { free(*payload); return -1; }
    if (masked) for (size_t i=0;i<length;i++) (*payload)[i]^=mask[i%4];
    (*payload)[length] = '\0';
    *plen = length;
    return 0;
}

static void ws_write_frame(int fd, int opcode, const void *payload, size_t len) {
    uint8_t hdr[10]; int hlen;
    hdr[0] = 0x80 | (opcode & 0x0f);
    if      (len <= 125)   { hdr[1]=len; hlen=2; }
    else if (len <= 65535) { hdr[1]=126; hdr[2]=len>>8; hdr[3]=len&0xff; hlen=4; }
    else                   { hdr[1]=127; for(int i=0;i<8;i++) hdr[2+i]=(len>>((7-i)*8))&0xff; hlen=10; }
    send_all(fd, hdr, hlen);
    if (len) send_all(fd, payload, len);
}

/*
 * httpd_ws_recv_frame — called twice by wshandler:
 *
 *   Call 1: max_len=0   → just fill f->type and f->len (the payload is already
 *                         in ctx->ws_payload from the frame-loop below)
 *   Call 2: max_len>0   → copy up to max_len bytes into f->payload
 */
esp_err_t httpd_ws_recv_frame(httpd_req_t *r, httpd_ws_frame_t *f, size_t max_len) {
    conn_ctx_t *c = ctx(r);
    if (!c->ws_payload) return ESP_FAIL;

    f->type = (c->ws_opcode == 2) ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
    f->len  = c->ws_payload_len;

    if (max_len == 0) return ESP_OK;   /* peek only */

    /* Copy payload */
    size_t copy = (c->ws_payload_len <= max_len) ? c->ws_payload_len : max_len;
    if (f->payload) memcpy(f->payload, c->ws_payload, copy);
    /* Leave ws_payload intact — wshandler may call recv_frame multiple times
       for multi-part binary uploads; caller responsible for freeing? No —
       in practice it's called exactly twice per frame in main.c, so free now. */
    free(c->ws_payload);
    c->ws_payload     = NULL;
    c->ws_payload_len = 0;
    return ESP_OK;
}

esp_err_t httpd_ws_send_frame(httpd_req_t *r, httpd_ws_frame_t *f) {
    conn_ctx_t *c = ctx(r);
    ws_write_frame(c->fd, f->type, f->payload, f->len);
    return ESP_OK;
}

/* ── Connection thread ──────────────────────────────────────────── */
typedef struct { server_t *srv; int fd; } conn_arg_t;

/* Read HTTP request headers into buf (null-terminated).
   Returns total bytes read, or -1 on error. */
static int read_http_headers(int fd, char *buf, size_t bufsz) {
    int pos = 0;
    while (pos < (int)bufsz - 1) {
        if (recv_exact(fd, buf+pos, 1) < 0) return -1;
        pos++;
        if (pos >= 4 && memcmp(buf+pos-4, "\r\n\r\n", 4) == 0) break;
    }
    buf[pos] = '\0';
    return pos;
}

/* Connection handler */
static void *conn_thread(void *arg) {
    conn_arg_t ca = *(conn_arg_t*)arg;  /* copy struct before freeing */
    free(arg);
    server_t *srv = ca.srv;
    int       fd  = ca.fd;

    char hbuf[4096];
    if (read_http_headers(fd, hbuf, sizeof hbuf) < 0) { close(fd); return NULL; }

    char method[16], uri_buf[512], version[16];
    if (sscanf(hbuf, "%15s %511s %15s", method, uri_buf, version) != 3) {
        close(fd); return NULL;
    }

    /* Set up per-request context */
    conn_ctx_t c = {0};
    c.fd = fd;
    strncpy(c.status, "200 OK", sizeof c.status - 1);

    httpd_req_t req = {0};
    req.uri      = uri_buf;
    req.method   = HTTP_GET;
    req._fd      = fd;
    req._ws_ctx  = &c;

    /* Check for WebSocket upgrade */
    char *hdrs   = strstr(hbuf, "\r\n");
    if (hdrs) hdrs += 2;
    else      hdrs  = hbuf;

    bool is_ws = false;
    char ws_key[128] = {0};
    char *upgrade = strcasestr(hdrs, "Upgrade:");
    if (upgrade && strcasestr(upgrade, "websocket")) {
        is_ws = true;
        char *kline = strcasestr(hdrs, "Sec-WebSocket-Key:");
        if (kline) {
            kline += 18;
            while (*kline == ' ') kline++;
            int ki = 0;
            while (*kline && *kline != '\r' && *kline != '\n' && ki < 127)
                ws_key[ki++] = *kline++;
        }
    }

    /* Split URI into path (for routing) and full URI (for handler) */
    char uri_path[512];
    strncpy(uri_path, uri_buf, sizeof uri_path - 1);
    uri_path[sizeof uri_path - 1] = '\0';
    char *qmark = strchr(uri_path, '?');
    if (qmark) *qmark = '\0';  /* strip query for routing only */

    /* Find matching URI handler — match on path, pass full URI to handler */
    httpd_uri_handler_t handler     = NULL;
    bool                is_ws_route = false;
    for (int i = 0; i < srv->n_uris; i++) {
        if (httpd_uri_match_wildcard(srv->uris[i].uri, uri_path, strlen(uri_path))) {
            handler     = srv->uris[i].handler;
            is_ws_route = srv->uris[i].is_websocket;
            break;
        }
    }

    if (!handler) {
        if (srv->err404) {
            srv->err404(&req, HTTPD_404_NOT_FOUND);
        } else {
            const char *r404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send_all(fd, r404, strlen(r404));
        }
        close(fd); return NULL;
    }

    if (is_ws && is_ws_route) {
        /* ── WebSocket upgrade ── */
        const char *MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        char combined[256];
        snprintf(combined, sizeof combined, "%s%s", ws_key, MAGIC);
        sha1_t sc; sha1_init(&sc);
        sha1_feed(&sc, combined, strlen(combined));
        uint8_t digest[20]; sha1_done(&sc, digest);
        char accept[32]; b64_enc(digest, 20, accept);

        char resp[512];
        int rlen = snprintf(resp, sizeof resp,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
        send_all(fd, resp, rlen);
        ESP_LOGI("httpd", "WS upgraded: %s", uri_buf);

        /* Notify handler of new WS connection (HTTP_GET method = connect) */
        req.method = HTTP_GET;
        handler(&req);

        /* WS frame loop */
        for (;;) {
            int      opcode;
            uint8_t *payload;
            size_t   plen;
            int wrf_ret = ws_read_frame(fd, &opcode, &payload, &plen);
            if (wrf_ret < 0) break;

            if (opcode == 8) { free(payload); break; }           /* close */
            if (opcode == 9) { ws_write_frame(fd,10,payload,plen); free(payload); continue; } /* pong */

            /* Stash for httpd_ws_recv_frame */
            c.ws_payload     = payload;   /* handler must NOT free this */
            c.ws_payload_len = plen;
            c.ws_opcode      = opcode;
            req.method       = -1;        /* not a fresh GET */
            esp_err_t rc = handler(&req);

            /* Clean up if handler didn't consume the payload */
            if (c.ws_payload) { free(c.ws_payload); c.ws_payload = NULL; }
            if (rc != ESP_OK) break;
        }
    } else {
        /* ── Plain HTTP ── */
        handler(&req);
    }

    close(fd);
    return NULL;
}

/* ── Public accept loop — call after app_main() ─────────────────── */
void httpd_run_forever(httpd_handle_t hdl) {
    server_t *srv = hdl;
    ESP_LOGI("httpd", "Ready — accepting on port %d", srv->port);
    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof ca;
        int cfd = accept(srv->listen_fd, (struct sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        ESP_LOGI("httpd", "Connection from %s:%d", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));

        conn_arg_t *arg = malloc(sizeof *arg);
        arg->srv = srv; arg->fd = cfd;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &attr, conn_thread, arg);
        pthread_attr_destroy(&attr);
    }
}
