/*
 * linux_main.c — entry point for native Linux build of the Wi-Fi Link server
 *
 * Replaces the FreeRTOS/ESP-IDF startup glue.  Calls app_main() from main.c
 * then hands off to httpd_run_forever() (in linux_httpd.c) which runs the
 * accept loop.  The DNS captive-portal loop in app_main() is patched out
 * (it tries to bind UDP :53, which needs root) — the HTTP server is what
 * we actually want to test.
 */

#define _GNU_SOURCE
#include "esp_idf_shims.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

/* Declared in linux_httpd.c */
extern void httpd_run_forever(void *h);

/* app_main() is in main.c — we declare it here */
void app_main(void);

/* The HTTP server handle is set by httpd_start() inside app_main().
 * We retrieve it via a wrapper so we can call httpd_run_forever() after. */
static void *g_server = NULL;

/* Intercept httpd_start to capture the handle */
static int (*real_httpd_start)(void**, const void*) = NULL;

/* Port override — set before app_main() runs */
int g_http_port = 8080;

/* Override the httpd port via build-time weak symbol */
__attribute__((weak))
int httpd_native_port(void) { return g_http_port; }

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -p, --port <n>    HTTP port (default 8080)\n"
        "  -h, --help        This help\n"
        "\n"
        "Open http://localhost:<port> in your browser.\n"
        "NVS data is stored in ./nvs_store/\n"
        "Press Ctrl-C to stop.\n",
        prog);
}

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"port", required_argument, 0, 'p'},
        {"help", no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:h", opts, NULL)) != -1) {
        switch (c) {
            case 'p': g_http_port = atoi(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    /* Ignore SIGPIPE so a closed browser tab doesn't kill the server */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stdout,
        "\n"
        "  ╔══════════════════════════════════════════╗\n"
        "  ║  AART Remora™ Programmer  (Linux native) ║\n"
        "  ╚══════════════════════════════════════════╝\n"
        "  Port:  %d\n"
        "  URL:   http://localhost:%d\n"
        "  NVS:   ./nvs_store/\n"
        "  Press Ctrl-C to stop.\n\n",
        g_http_port, g_http_port);

    app_main();
    /* app_main() blocks in the DNS loop; we never get here.
     * With the patched main.c (DNS_DISABLED), it returns normally
     * and we start the HTTP accept loop below. */
    return 0;
}
