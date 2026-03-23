/*
 * test_ws.c — End-to-end WS test using socketpair() (no network needed)
 *
 * Spawns conn_thread on one end of a socketpair and acts as a WS client
 * on the other end, verifying the full request/response cycle.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "esp_idf_shims.h"

extern void app_main(void);
extern void httpd_run_forever(httpd_handle_t h);

static int passed = 0, failed = 0;
#define PASS(msg)       do { printf("  PASS  %s\n", msg); passed++; } while(0)
#define FAIL(msg, got)  do { printf("  FAIL  %s  [got: %.80s]\n", msg, got?got:"<null>"); failed++; } while(0)
#define CHECK_IN(msg, hay, needle) do { \
    if ((hay) && strstr((hay),(needle))) PASS(msg); \
    else FAIL(msg, hay); \
} while(0)

/* ── I/O helpers ────────────────────────────────────────────────── */
static void send_all_fd(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) { ssize_t w = write(fd,p,n); if(w<=0) break; p+=w; n-=w; }
}

static int recv_exact_fd(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

/* ── WS client helpers ──────────────────────────────────────────── */
/* RFC 6455 client-side masked frame */
static void ws_send(int fd, int opcode, const void *data, size_t n) {
    uint8_t mask[4] = {0xab,0xcd,0xef,0x01};
    uint8_t *masked = malloc(n);
    for (size_t i=0;i<n;i++) masked[i]=((uint8_t*)data)[i]^mask[i%4];
    uint8_t hdr[8]; int hlen;
    hdr[0] = 0x80 | (opcode&0x0f);
    if (n <= 125)      { hdr[1]=0x80|n; hlen=2; }
    else if (n<=65535) { hdr[1]=0xfe; hdr[2]=n>>8; hdr[3]=n&0xff; hlen=4; }
    else               { hdr[1]=0xff; for(int i=0;i<8;i++) hdr[i]=(n>>((7-i)*8))&0xff; hlen=10; }
    /* append mask after length bytes */
    send_all_fd(fd, hdr, hlen);
    send_all_fd(fd, mask, 4);
    send_all_fd(fd, masked, n);
    free(masked);
}

static void ws_send_text(int fd, const char *s) {
    ws_send(fd, 1, s, strlen(s));
}

/* Read one unmasked server frame, return heap-alloc'd payload (caller frees).
   Returns NULL on error. */
static char *ws_recv_frame(int fd, int *opcode_out) {
    uint8_t h2[2];
    if (recv_exact_fd(fd, h2, 2) < 0) return NULL;
    if (opcode_out) *opcode_out = h2[0] & 0x0f;
    size_t len = h2[1] & 0x7f;
    if (len == 126) {
        uint8_t e[2]; if(recv_exact_fd(fd,e,2)<0) return NULL;
        len = ((size_t)e[0]<<8)|e[1];
    } else if (len == 127) {
        uint8_t e[8]; if(recv_exact_fd(fd,e,8)<0) return NULL;
        len=0; for(int i=0;i<8;i++) len=(len<<8)|e[i];
    }
    char *buf = malloc(len+2);
    if (!buf) return NULL;
    if (len && recv_exact_fd(fd, buf, len)<0) { free(buf); return NULL; }
    buf[len] = '\0';
    buf[len+1] = '\0';  /* double null so strlen-based code sees end */
    /* Replace embedded nulls with spaces so strstr works on full frame */
    for (size_t i=0; i<len; i++) if (buf[i]=='\0') buf[i]=' ';
    return buf;
}

/* Accumulate text frames until OK\n or ERROR\n seen.
   Returns heap-alloc'd concatenated payload. */
static char *ws_recv_response(int fd) {
    char *accum = calloc(1, 65536);
    size_t pos = 0;
    for (int i=0; i<30; i++) {
        int op;
        char *frame = ws_recv_frame(fd, &op);
        if (!frame) break;
        if (op == 8) { free(frame); break; }
        size_t flen = strlen(frame);  /* safe after null-replacement above */
        if (pos + flen < 65535) { memcpy(accum+pos, frame, flen); pos+=flen; }
        free(frame);
        if (strstr(accum,"OK\n") || strstr(accum,"ERROR\n")) break;
    }
    return accum;
}

/* ── WS handshake ────────────────────────────────────────────────── */
static bool do_ws_handshake(int fd) {
    const char *req =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    send_all_fd(fd, req, strlen(req));

    char resp[1024]; int pos=0;
    while (pos < 1023) {
        if (recv_exact_fd(fd, resp+pos, 1) < 0) return false;
        pos++;
        if (pos>=4 && memcmp(resp+pos-4,"\r\n\r\n",4)==0) break;
    }
    resp[pos]='\0';
    return strstr(resp,"101 Switching") != NULL;
}

/* ── HTTP helpers ────────────────────────────────────────────────── */
static char http_resp_buf[131072];
static char *do_http_get(int fd, const char *path) {
    char req[512];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    send_all_fd(fd, req, n);

    /* Read headers */
    int pos=0;
    while (pos < (int)sizeof(http_resp_buf)-1) {
        ssize_t r = read(fd, http_resp_buf+pos, 1);
        if (r<=0) break;
        pos++;
        if (pos>=4 && memcmp(http_resp_buf+pos-4,"\r\n\r\n",4)==0) break;
    }
    /* Content-Length */
    char *cl = strcasestr(http_resp_buf, "Content-Length:");
    size_t body_len = cl ? (size_t)atoi(cl+15) : 0;
    /* Read body */
    char *body_start = strstr(http_resp_buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t already = (http_resp_buf+pos) - body_start;
        while (already < body_len) {
            ssize_t r = read(fd, http_resp_buf+pos, sizeof(http_resp_buf)-pos-1);
            if (r<=0) break;
            pos+=r; already+=r;
        }
    }
    http_resp_buf[pos]='\0';
    return http_resp_buf;
}

/* ── conn_thread shim — we need to call it with a server handle ─── */
/* Import internal type from linux_httpd.c — must match exactly */
typedef struct {
    int  listen_fd;
    int  port;
    int  n_uris;
    httpd_uri_t uris[16]; /* MAX_HANDLERS */
    httpd_err_handler_t err404;
} server_t_mirror;

typedef struct { server_t_mirror *srv; int fd; } conn_arg_mirror;

/* conn_thread is static in linux_httpd.c, so we can't call it directly.
   Instead use a socketpair + httpd_run_forever in a thread. */

static httpd_handle_t g_server = NULL;
static int            g_port   = 0;

int g_http_port = 19191;  /* defined here for test binary */

static void *server_thread(void *arg) {
    (void)arg;
    app_main();
    return NULL;
}

static int open_conn_to_server(void) {
    /* Connect to the real listening socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(g_http_port),
    };
    inet_aton("127.0.0.1", &sa.sin_addr);
    int retries = 0;
    while (connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0 && retries++ < 20)
        usleep(50000);
    /* 5 second recv timeout so tests don't hang forever */
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* ── main ────────────────────────────────────────────────────────── */
int main(void) {
    printf("\n=== Native binary end-to-end tests ===\n\n");

    /* Start app_main in a background thread */
    g_http_port = 19191;
    pthread_t srv_thr;
    pthread_create(&srv_thr, NULL, server_thread, NULL);
    usleep(300000); /* wait for server to bind */

    /* ── HTTP tests ── */
    printf("--- HTTP routes ---\n");
    {
        int fd = open_conn_to_server();
        char *resp = do_http_get(fd, "/");
        CHECK_IN("GET /  → 200", resp, "200");
        CHECK_IN("GET /  → gzip Content-Encoding", resp, "gzip");
        close(fd);
    }
    {
        int fd = open_conn_to_server();
        char *resp = do_http_get(fd, "/?en");
        CHECK_IN("GET /?en  → 200", resp, "200");
        close(fd);
    }
    {
        int fd = open_conn_to_server();
        char *resp = do_http_get(fd, "/?de");
        CHECK_IN("GET /?de  → 200", resp, "200");
        close(fd);
    }
    {
        int fd = open_conn_to_server();
        char *resp = do_http_get(fd, "/preset/nosuchpreset.json");
        CHECK_IN("GET /preset/missing → 404", resp, "404");
        close(fd);
    }
    {
        int fd = open_conn_to_server();
        char *resp = do_http_get(fd, "/anythingelse");
        CHECK_IN("GET /other → 302 redirect", resp, "302");
        close(fd);
    }

    /* ── WebSocket tests ── */
    printf("\n--- WebSocket command processing ---\n");
    {
        int fd = open_conn_to_server();
        if (!do_ws_handshake(fd)) {
            FAIL("WS handshake", "101 not received");
        } else {
            PASS("WS handshake → 101");

            ws_send_text(fd, "show\n");
            char *r = ws_recv_response(fd);
            CHECK_IN("show → freq_min present",   r, "freq_min");
            CHECK_IN("show → duty_max present",    r, "duty_max");
            CHECK_IN("show → ends with OK",        r, "OK\n");
            free(r);

            ws_send_text(fd, "get timing\n");
            r = ws_recv_response(fd);
            CHECK_IN("get timing → key:val line",  r, "timing:");
            CHECK_IN("get timing → OK",            r, "OK\n");
            free(r);

            ws_send_text(fd, "set timing 22\n");
            r = ws_recv_response(fd);
            CHECK_IN("set timing → OK",            r, "OK\n");
            free(r);

            ws_send_text(fd, "get timing\n");
            r = ws_recv_response(fd);
            CHECK_IN("get timing after set → 22",  r, "timing: 22");
            free(r);

            ws_send_text(fd, "save\n");
            r = ws_recv_response(fd);
            CHECK_IN("save → OK",                  r, "OK\n");
            free(r);

            ws_send_text(fd, "reset\n");
            r = ws_recv_response(fd);
            CHECK_IN("reset → OK",                 r, "OK\n");
            free(r);

            ws_send_text(fd, "_probe\n");
            r = ws_recv_response(fd);
            CHECK_IN("_probe → result line",       r, "_probe");
            free(r);

            ws_send_text(fd, "_info\n");
            r = ws_recv_response(fd);
            CHECK_IN("_info → OK",                 r, "OK\n");
            free(r);

            ws_send_text(fd, "_wifi_get\n");
            r = ws_recv_response(fd);
            CHECK_IN("_wifi_get → OK",             r, "OK\n");
            free(r);

            ws_send_text(fd, "_wifi_set TestSSID\tTestPass\n");
            r = ws_recv_response(fd);
            CHECK_IN("_wifi_set → OK",             r, "OK\n");
            free(r);

            ws_send_text(fd, "_wifi_get\n");
            r = ws_recv_response(fd);
            CHECK_IN("_wifi_get shows new SSID",   r, "TestSSID");
            free(r);

            ws_send_text(fd, "badcmd xyz\n");
            r = ws_recv_response(fd);
            CHECK_IN("unknown cmd → ERROR",        r, "ERROR\n");
            free(r);
        }
        close(fd);
    }

    /* ── NVS persistence test ── */
    printf("\n--- NVS persistence ---\n");
    {
        nvs_handle_t h;
        nvs_open("test", NVS_READWRITE, &h);
        nvs_set_str(h, "k1", "hello"); nvs_commit(h); nvs_close(h);
        nvs_open("test", NVS_READONLY, &h);
        char val[32]; size_t len = sizeof val;
        esp_err_t e = nvs_get_str(h, "k1", val, &len);
        nvs_close(h);
        if (e == ESP_OK && strcmp(val,"hello")==0) PASS("NVS round-trip");
        else { const char *_v = val; FAIL("NVS round-trip", _v); }
    }

    /* ── CRC32 ── */
    printf("\n--- CRC32 ---\n");
    {
        uint8_t data[] = "123456789";
        uint32_t crc = esp_crc32_le(0, data, 9);
        if (crc == 0xCBF43926) PASS("CRC32('123456789') == 0xCBF43926");
        else { char tmp[32]; const char *_t=tmp; sprintf(tmp,"0x%08X",crc); FAIL("CRC32",_t); }
    }

    printf("\n========================================\n");
    printf("  %d passed,  %d failed\n", passed, failed);
    printf("========================================\n");
    return failed == 0 ? 0 : 1;
}
