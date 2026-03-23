/*
 * mock_uart.h — Software mock of the ESCape32 UART interface
 *
 * Implements two layers:
 *   1. Text commands  (show, get, set, save, reset, throt, play)
 *      → ESCape32 processes these as ASCII lines, responds with ASCII
 *   2. Binary protocol (CMD_PROBE, CMD_INFO, CMD_READ/WRITE/UPDATE, CMD_SETWRP)
 *      → used for firmware update and bootloader interaction
 *
 * A background thread reads from the host-to-ESC pipe, dispatches both
 * text and binary commands, and writes responses to the ESC-to-host pipe.
 */
#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/* ── Mock ESC parameter state ────────────────────────────────────── */
typedef struct {
    const char *key;
    int         val;
} mock_param_t;

static mock_param_t _esc_params[] = {
    {"arm",0},{"damp",1},{"revdir",0},{"brushed",0},
    {"timing",14},{"sine_range",25},{"sine_power",25},
    {"freq_min",48},{"freq_max",48},
    {"duty_min",100},{"duty_max",100},{"duty_spup",30},{"duty_ramp",50},
    {"duty_rate",35},{"duty_drag",0},{"duty_lock",0},
    {"throt_mode",0},{"throt_rev",0},{"throt_brk",25},
    {"throt_set",0},{"throt_ztc",0},{"throt_cal",1},
    {"throt_min",0},{"throt_mid",1000},{"throt_max",2000},
    {"analog_min",0},{"analog_max",1440},
    {"input_mode",1},{"input_ch1",0},{"input_ch2",1},
    {"telem_mode",0},{"telem_phid",1},{"telem_poles",14},
    {"prot_stall",0},{"prot_temp",80},{"prot_sens",0},
    {"prot_volt",33},{"prot_cells",0},{"prot_curr",0},
    {"music",0},{"volume",25},{"beacon",25},{"bec",0},{"led",0},
    {NULL,0}
};

static int _esc_get(const char *key) {
    for (int i=0; _esc_params[i].key; i++)
        if (!strcmp(_esc_params[i].key, key)) return _esc_params[i].val;
    return -1;
}
static void _esc_set(const char *key, int val) {
    for (int i=0; _esc_params[i].key; i++)
        if (!strcmp(_esc_params[i].key, key)) { _esc_params[i].val=val; return; }
}

/* ── Pipes ───────────────────────────────────────────────────────── */
static int _esc_rx[2] = {-1,-1}; /* host→esc: host writes TX, esc reads  */
static int _esc_tx[2] = {-1,-1}; /* esc→host: esc writes,    host reads  */
static pthread_t     _esc_thread;
static pthread_once_t _esc_once = PTHREAD_ONCE_INIT;

static void _p_write(const void *buf, size_t n) {
    const char *p=buf;
    while(n>0){ssize_t w=write(_esc_tx[1],p,n);if(w<=0)break;p+=w;n-=w;}
}
static int _p_read(void *buf, size_t n) {
    uint8_t *p=buf;
    while(n>0){ssize_t r=read(_esc_rx[0],p,n);if(r<=0)return -1;p+=r;n-=r;}
    return 0;
}

/* Read one raw byte */
static int _p_getbyte(void) {
    uint8_t b;
    if (_p_read(&b,1)<0) return -1;
    return b;
}

/* ── CRC32 ───────────────────────────────────────────────────────── */
static uint32_t _crc32(const uint8_t *d, size_t n) {
    uint32_t c=0xFFFFFFFF;
    for(size_t i=0;i<n;i++){c^=d[i];for(int j=0;j<8;j++)c=(c>>1)^(0xEDB88320&-(c&1));}
    return c^0xFFFFFFFF;
}

/* ── Binary protocol helpers ─────────────────────────────────────── */
static int _bin_recvval(void) {
    uint8_t b[2]; if(_p_read(b,2)<0)return -1;
    return (b[0]^b[1])==0xff ? b[0] : -1;
}
static void _bin_sendval(uint8_t v) {
    uint8_t b[2]={v,(uint8_t)~v}; _p_write(b,2);
}
static int _bin_recvdata(uint8_t *out, int max) {
    int cnt=_bin_recvval(); if(cnt<0)return -1;
    int len=(cnt+1)*4; if(len>max)return -1;
    if(_p_read(out,len)<0)return -1;
    uint32_t crc; if(_p_read(&crc,4)<0)return -1;
    return (_crc32(out,len)==crc)?len:-1;
}
static void _bin_senddata(const uint8_t *d, int len) {
    _bin_sendval((len>>2)-1);
    _p_write(d,len);
    uint32_t crc=_crc32(d,len);
    _p_write(&crc,4);
}

/* ── Binary command handlers ─────────────────────────────────────── */
#define BCMD_PROBE  0
#define BCMD_INFO   1
#define BCMD_READ   2
#define BCMD_WRITE  3
#define BCMD_UPDATE 4
#define BCMD_SETWRP 5

static void _handle_binary(int cmd) {
    switch(cmd) {
        case BCMD_PROBE:
            _bin_sendval(0); /* OK */
            break;
        case BCMD_INFO: {
            uint8_t info[32]={0};
            /* byte[0]=boot major, [1]=boot minor, [2..5]=MCU ID */
            info[0]=1; info[1]=5;
            info[2]=0x21; info[3]=0x04; info[4]=0x25; info[5]=0x02;
            _bin_senddata(info,32);
            break;
        }
        case BCMD_READ: {
            int blk=_bin_recvval(); (void)blk;
            int cnt=_bin_recvval(); if(cnt<0)break;
            int len=(cnt+1)*4;
            uint8_t block[20]={0};
            /* Firmware header magic: 0xea 0x32 */
            block[0]=0xea; block[1]=0x32;
            block[2]=99; /* fw version */
            /* Target string at offset 4 */
            const char *tgt="Remora2";
            memcpy(block+4,tgt,strlen(tgt)+1);
            _bin_senddata(block, len<20?len:20);
            break;
        }
        case BCMD_WRITE: {
            _bin_recvval(); /* block number */
            uint8_t tmp[1028]; _bin_recvdata(tmp,sizeof tmp);
            _bin_sendval(0);
            break;
        }
        case BCMD_UPDATE: {
            uint8_t tmp[1028]; _bin_recvdata(tmp,sizeof tmp);
            _bin_sendval(0);
            _bin_sendval(0); /* post-reboot ACK */
            break;
        }
        case BCMD_SETWRP: {
            _bin_recvval(); /* wrp value */
            _bin_sendval(0);
            break;
        }
        default:
            fprintf(stderr,"[mock_esc] unknown binary cmd 0x%02x\n",cmd);
            break;
    }
}

/* ── Text command line accumulator + dispatcher ──────────────────── */
static void _dispatch_text(const char *line) {
    char resp[4096];
    char key[64]=""; int val=0;

    if (!strcmp(line,"show")) {
        int n=snprintf(resp,sizeof resp,"show\n");
        for(int i=0;_esc_params[i].key;i++)
            n+=snprintf(resp+n,sizeof resp-n,"%s: %d\n",_esc_params[i].key,_esc_params[i].val);
        n+=snprintf(resp+n,sizeof resp-n,"OK\n");
        _p_write(resp,n);
    } else if (sscanf(line,"get %63s",key)==1) {
        int v=_esc_get(key);
        int n=snprintf(resp,sizeof resp,"get %s\n%s: %d\nOK\n",key,key,v);
        _p_write(resp,n);
    } else if (sscanf(line,"set %63s %d",key,&val)==2) {
        _esc_set(key,val);
        int n=snprintf(resp,sizeof resp,"set %s\n%s: %d\nOK\n",key,key,val);
        _p_write(resp,n);
    } else if (!strcmp(line,"save")) {
        int n=snprintf(resp,sizeof resp,"save\nOK\n");
        _p_write(resp,n);
    } else if (!strcmp(line,"reset")) {
        /* Reset a few params to defaults */
        _esc_set("timing",0); _esc_set("damp",0); _esc_set("revdir",0);
        int n=snprintf(resp,sizeof resp,"reset\nOK\n");
        _p_write(resp,n);
    } else if (!strncmp(line,"throt ",6)) {
        int n=snprintf(resp,sizeof resp,"throt %s\nOK\n",line+6);
        _p_write(resp,n);
    } else if (!strncmp(line,"play ",5)) {
        /* Don't respond - wshandler returns immediately for play */
        (void)0;
    } else if (!strncmp(line,"info",4)) {
        int n=snprintf(resp,sizeof resp,"info\n12345 rpm  0.0A\nOK\n");
        _p_write(resp,n);
    } else {
        int n=snprintf(resp,sizeof resp,"%s\nERROR\n",line);
        _p_write(resp,n);
    }
}

/* ── Main ESC thread ─────────────────────────────────────────────── */
static void *_esc_run(void *arg) {
    (void)arg;
    char linebuf[512]; int lpos=0;
    for(;;) {
        int b=_p_getbyte();
        if(b<0) break;

        /* Binary commands are single-byte values 0x00-0x05 sent as val+~val pairs.
           A text command character will be printable ASCII (0x20-0x7e).
           We peek: if byte <= 0x05 and next byte == ~b, treat as binary.
           Otherwise accumulate as text. */
        if (b <= 0x05) {
            /* Could be binary - read second byte to confirm */
            int b2=_p_getbyte();
            if(b2<0) break;
            if((uint8_t)(b^b2)==0xff) {
                /* Valid binary command */
                _handle_binary(b);
                lpos=0; /* reset text accumulator */
            } else {
                /* False positive - treat both as text chars */
                if(lpos<(int)sizeof linebuf-1) linebuf[lpos++]=(char)b;
                if(lpos<(int)sizeof linebuf-1) linebuf[lpos++]=(char)b2;
            }
        } else if(b=='\n') {
            linebuf[lpos]='\0';
            /* Strip trailing \r */
            if(lpos>0 && linebuf[lpos-1]=='\r') linebuf[--lpos]='\0';
            if(lpos>0) _dispatch_text(linebuf);
            lpos=0;
        } else {
            if(lpos<(int)sizeof linebuf-1) linebuf[lpos++]=(char)b;
        }
    }
    return NULL;
}

static void _mock_uart_init(void) {
    if(pipe(_esc_rx)<0||pipe(_esc_tx)<0){perror("pipe");return;}
    pthread_create(&_esc_thread,NULL,_esc_run,NULL);
    pthread_detach(_esc_thread);
}

/* ── Override UART inline stubs ──────────────────────────────────── */

static inline esp_err_t uart_driver_install(int p,int b,int e,int q,
                                             QueueHandle_t *qh,int f) {
    (void)p;(void)b;(void)e;(void)q;(void)qh;(void)f;
    pthread_once(&_esc_once,_mock_uart_init);
    return ESP_OK;
}
static inline esp_err_t uart_param_config(int p,const uart_config_t *c){(void)p;(void)c;return ESP_OK;}
static inline esp_err_t uart_set_pin(int p,int a,int b,int c,int d){(void)p;(void)a;(void)b;(void)c;(void)d;return ESP_OK;}
static inline esp_err_t uart_set_mode(int p,int m){(void)p;(void)m;return ESP_OK;}

static inline esp_err_t uart_get_buffered_data_len(int p, size_t *s) {
    (void)p; *s=0;
    if(_esc_tx[0]<0) return ESP_OK;
    int avail=0;
    ioctl(_esc_tx[0],FIONREAD,&avail);
    *s=(size_t)(avail>0?avail:0);
    return ESP_OK;
}
static inline int uart_read_bytes(int p,void *buf,size_t n,int t) {
    (void)p;(void)t;
    if(_esc_tx[0]<0) return 0;
    ssize_t r=read(_esc_tx[0],buf,n);
    return r>0?(int)r:0;
}
static inline int uart_write_bytes(int p,const void *buf,size_t n) {
    (void)p;
    if(_esc_rx[1]<0) return (int)n;
    ssize_t w=write(_esc_rx[1],buf,n);
    return w>0?(int)w:0;
}
static inline esp_err_t uart_flush(int p) {
    (void)p;
    if(_esc_tx[0]>=0) {
        char tmp[256]; int fl=fcntl(_esc_tx[0],F_GETFL,0);
        fcntl(_esc_tx[0],F_SETFL,fl|O_NONBLOCK);
        while(read(_esc_tx[0],tmp,sizeof tmp)>0);
        fcntl(_esc_tx[0],F_SETFL,fl);
    }
    return ESP_OK;
}

/* xQueueReceive: block-wait on the esc→host pipe using select() */
static inline int xQueueReceive(QueueHandle_t q, void *e, int t) {
    (void)q;(void)t;
    uart_event_t *ev=(uart_event_t*)e;
    if(_esc_tx[0]<0){usleep(200000);return 0;}
    struct timeval tv={0,200000};
    fd_set fds; FD_ZERO(&fds); FD_SET(_esc_tx[0],&fds);
    int r=select(_esc_tx[0]+1,&fds,NULL,NULL,&tv);
    if(r<=0) return 0;
    int avail=1; ioctl(_esc_tx[0],FIONREAD,&avail);
    ev->size=(size_t)(avail>0?avail:1);
    ev->type=UART_DATA;
    return 1;
}
