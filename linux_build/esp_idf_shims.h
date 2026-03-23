/*
 * esp_idf_shims.h  —  Minimal ESP-IDF API stubs for native Linux/macOS build
 *
 * Lets main.c compile and run natively so the HTTP server, WebSocket handler,
 * DNS logic, NVS storage and command processing can all be tested without
 * an ESP32 or QEMU.
 *
 * Compile with:   make -f Makefile.linux
 * Run with:       ./wifi_link_native [--port 80]
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

/* ── ESP error codes ────────────────────────────────────── */
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL       -1
#define ESP_ERR_NOT_FOUND  0x105
#define ESP_ERROR_CHECK(x) do { \
    esp_err_t _rc = (x); \
    if (_rc != ESP_OK) { \
        fprintf(stderr, "ESP_ERROR_CHECK failed: %d at %s:%d\n", _rc, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

/* ── Logging ────────────────────────────────────────────── */
#define ESP_LOGI(tag, fmt, ...) fprintf(stdout, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stdout, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  /* debug suppressed */
#define ESP_LOG_NONE    0
#define ESP_LOG_ERROR   1
#define ESP_LOG_WARN    2
#define ESP_LOG_INFO    3
#define ESP_LOG_DEBUG   4
#define ESP_LOG_VERBOSE 5
static inline void esp_log_level_set(const char *tag, int level) { (void)tag; (void)level; }

/* ── FreeRTOS stubs ─────────────────────────────────────── */
typedef void *QueueHandle_t;
typedef struct { int type; size_t size; } uart_event_t;
#define UART_DATA          1
#define portMAX_DELAY      0xFFFFFFFF
#define portTICK_PERIOD_MS 1

static inline QueueHandle_t xQueueCreate(int n, int s) { (void)n;(void)s; return malloc(1); }
#ifndef MOCK_UART_OVERRIDE
static inline int  xQueueReceive(QueueHandle_t q, void *e, int t) { (void)q;(void)e;(void)t; usleep(200000); return 0; }
#endif
static inline void xQueueReset(QueueHandle_t q)  { (void)q; }
static inline void vTaskDelay(int t)             { usleep(t * 1000); }

/* ── GPIO stubs ─────────────────────────────────────────── */
#define GPIO_MODE_OUTPUT 1
#define CONFIG_LED_PIN   2
static inline esp_err_t gpio_set_direction(int p, int m) { (void)p;(void)m; return ESP_OK; }
static inline esp_err_t gpio_set_level(int p, int v)     { (void)p;(void)v; return ESP_OK; }

/* ── UART stubs ─────────────────────────────────────────── */
#define CONFIG_UART_NUM  1
#define CONFIG_UART_TX   33
#define CONFIG_UART_RX   16
#define UART_DATA_8_BITS  3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1  1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_MODE_RS485_HALF_DUPLEX 2
#define UART_HW_FIFO_LEN(n) 128
#define CONFIG_LWIP_MAX_SOCKETS 16
typedef struct { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
#ifndef MOCK_UART_OVERRIDE
static inline esp_err_t uart_driver_install(int p,int b,int e,int q,QueueHandle_t *qh,int f){(void)p;(void)b;(void)e;(void)q;(void)qh;(void)f;return ESP_OK;}
static inline esp_err_t uart_param_config(int p, const uart_config_t *c){(void)p;(void)c;return ESP_OK;}
static inline esp_err_t uart_set_pin(int p,int tx,int rx,int a,int b){(void)p;(void)tx;(void)rx;(void)a;(void)b;return ESP_OK;}
static inline esp_err_t uart_set_mode(int p, int m){(void)p;(void)m;return ESP_OK;}
static inline esp_err_t uart_get_buffered_data_len(int p, size_t *s){(void)p;*s=0;return ESP_OK;}
static inline int  uart_read_bytes(int p,void *b,size_t n,int t){(void)p;(void)b;(void)n;(void)t;return 0;}
static inline int  uart_write_bytes(int p,const void *b,size_t n){(void)p;(void)b;(void)n;return (int)n;}
static inline esp_err_t uart_flush(int p){(void)p;return ESP_OK;}

#endif /* MOCK_UART_OVERRIDE */

/* ── CRC ────────────────────────────────────────────────── */
static inline uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, size_t len) {
    /* Standard CRC32 (LE) */
    crc ^= 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ── NVS ────────────────────────────────────────────────── */
typedef uintptr_t nvs_handle_t;
#define NVS_READONLY  0
#define NVS_READWRITE 1

/* NVS backed by flat files in ./nvs_store/<namespace>/<key> */
#include <stdint.h>
#include <sys/stat.h>
static inline void _nvs_path(char *out, size_t n,
                              const char *ns, const char *key) {
    snprintf(out, n, "./nvs_store/%s/%s", ns, key);
}
static inline esp_err_t nvs_flash_init(void) {
    mkdir("./nvs_store", 0755);
    return ESP_OK;
}
static inline esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)mode;
    /* Store namespace index in handle as a hash-ish value */
    *h = (nvs_handle_t)(uintptr_t)strdup(ns);
    char dir[256];
    snprintf(dir, sizeof dir, "./nvs_store/%s", ns);
    mkdir(dir, 0755);
    return ESP_OK;
}
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *key,
                                     char *out, size_t *len) {
    const char *ns = (const char *)(uintptr_t)h;
    char path[512];
    _nvs_path(path, sizeof path, ns, key);
    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(out, 1, *len - 1, f);
    out[n] = '\0';
    *len = n + 1;
    fclose(f);
    return ESP_OK;
}
static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *key,
                                     const char *val) {
    const char *ns = (const char *)(uintptr_t)h;
    char path[512];
    _nvs_path(path, sizeof path, ns, key);
    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    fputs(val, f);
    fclose(f);
    return ESP_OK;
}
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    const char *ns = (const char *)(uintptr_t)h;
    char path[512];
    _nvs_path(path, sizeof path, ns, key);
    remove(path);
    return ESP_OK;
}
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { free((void *)(uintptr_t)h); }

/* ── Wi-Fi stubs (no-ops, networking handled by real sockets) ── */
typedef struct {} esp_netif_t;
#define WIFI_INIT_CONFIG_DEFAULT() {}
#define WIFI_MODE_AP 2
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define ESP_IF_WIFI_AP 1
typedef struct {
    struct { uint8_t ssid[33]; uint8_t password[65];
             uint8_t ssid_len; int max_connection; int authmode; } ap;
} wifi_config_t;
typedef struct {} wifi_init_config_t;
static inline esp_err_t esp_netif_init(void)               { return ESP_OK; }
static inline esp_netif_t* esp_netif_create_default_wifi_ap(void){ return NULL; }
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(int m)            { (void)m; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(int i, wifi_config_t *c){ (void)i;(void)c; return ESP_OK; }
static inline esp_err_t esp_wifi_start(void)                { return ESP_OK; }

/* ── Event loop stubs ───────────────────────────────────── */
typedef const char *esp_event_base_t;
#define ESP_HTTP_SERVER_EVENT NULL
#define HTTP_SERVER_EVENT_ON_CONNECTED  0
#define HTTP_SERVER_EVENT_DISCONNECTED  1
static inline esp_err_t esp_event_loop_create_default(void){ return ESP_OK; }
static inline esp_err_t esp_event_handler_register(
    esp_event_base_t b, int32_t id,
    void (*h)(void*,esp_event_base_t,int32_t,void*), void *a)
{ (void)b;(void)id;(void)h;(void)a; return ESP_OK; }

/* ── mDNS stubs ─────────────────────────────────────────── */
static inline esp_err_t mdns_init(void)                  { return ESP_OK; }
static inline esp_err_t mdns_hostname_set(const char *h) { (void)h; return ESP_OK; }

/* ── HTTP server ────────────────────────────────────────── */
/*
 * We replace the ESP-IDF httpd with a real POSIX TCP server that speaks
 * HTTP/1.1 and WebSocket.  The same handler function pointers from main.c
 * are called, but the httpd_req_t is backed by a real socket fd.
 */
typedef struct httpd_req {
    const char *uri;
    int         method;     /* HTTP_GET = 0 */
    int         _fd;        /* underlying socket */
    void       *_ws_ctx;    /* non-NULL when upgraded to WS */
} httpd_req_t;

#define HTTP_GET 0

typedef enum { HTTPD_404_NOT_FOUND = 0 } httpd_err_code_t;
typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t *);
typedef esp_err_t (*httpd_err_handler_t)(httpd_req_t *, httpd_err_code_t);

typedef struct { const char *uri; int method; httpd_uri_handler_t handler; bool is_websocket; } httpd_uri_t;
typedef bool (*httpd_uri_match_func_t)(const char*, const char*, size_t);
typedef struct {
    int max_open_sockets;
    bool lru_purge_enable;
    httpd_uri_match_func_t uri_match_fn;
    int server_port;
} httpd_config_t;
typedef void *httpd_handle_t;

/* WS frame */
#define HTTPD_WS_TYPE_TEXT   1
#define HTTPD_WS_TYPE_BINARY 2
typedef struct {
    int type; uint8_t *payload; size_t len; bool final; bool fragmented;
} httpd_ws_frame_t;

/* ── Timer stubs ─────────────────────────────────────────── */
typedef int TimerHandle_t;
static inline void clearInterval(int id)  { (void)id; }
static inline void clearTimeout(int id)   { (void)id; }

/* Forward declarations implemented in linux_httpd.c */
esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *cfg);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri);
esp_err_t httpd_register_err_handler(httpd_handle_t h, httpd_err_code_t e, httpd_err_handler_t fn);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_ws_recv_frame(httpd_req_t *r, httpd_ws_frame_t *f, size_t max);
esp_err_t httpd_ws_send_frame(httpd_req_t *r, httpd_ws_frame_t *f);
bool      httpd_uri_match_wildcard(const char *t, const char *u, size_t n);

#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){ \
    .max_open_sockets = 7, .lru_purge_enable = true, \
    .uri_match_fn = NULL, .server_port = 80 })


#ifdef MOCK_UART_OVERRIDE
#  include "mock_uart.h"
#endif

/* build_defs.h replacement — embed the gzipped files inline */
#include "build_defs.h"
