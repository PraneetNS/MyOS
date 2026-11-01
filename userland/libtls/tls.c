/*
 * userland/libtls/tls.c - TLS 1.2/1.3 client implementation for MyOS.
 * Built on BearSSL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "inc/bearssl.h"
#include "tls.h"

#define MIN_BUILD_EPOCH 1700000000L /* Epoch sanity check: clock must not be before build date */

struct tls_client {
    int fd;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_x509_trust_anchor* anchors;
    size_t num_anchors;
    int last_err;
};

static int sock_read(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    for (;;) {
        ssize_t r = read(fd, buf, len);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        return (int)r;
    }
}

static int sock_write(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    for (;;) {
        ssize_t w = write(fd, buf, len);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        return (int)w;
    }
}

/* Helper to clone memory */
static void* mem_dup(const void* src, size_t len) {
    if (!src || len == 0) return NULL;
    void* p = malloc(len);
    if (p) memcpy(p, src, len);
    return p;
}

/* Buffer for dynamic DN accumulation */
typedef struct {
    unsigned char* data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_append(void* ctx, const void* src, size_t len) {
    buf_t* b = (buf_t*)ctx;
    if (b->len + len > b->cap) {
        size_t ncap = (b->cap == 0) ? 256 : b->cap * 2;
        while (b->len + len > ncap) ncap *= 2;
        unsigned char* ndata = realloc(b->data, ncap);
        if (!ndata) return;
        b->data = ndata;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, src, len);
    b->len += len;
}

/* Parse DER X.509 certificate and append to trust anchor array */
static int parse_cert_to_anchor(const unsigned char* der, size_t der_len, br_x509_trust_anchor* ta) {
    br_x509_decoder_context dc;
    buf_t dn = {0};

    br_x509_decoder_init(&dc, buf_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) {
        free(dn.data);
        return -1;
    }

    ta->dn.data = dn.data;
    ta->dn.len = dn.len;
    ta->flags = 0;
    if (br_x509_decoder_isCA(&dc)) {
        ta->flags |= BR_X509_TA_CA;
    }

    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key_type = BR_KEYTYPE_RSA;
        ta->pkey.key.rsa.n = mem_dup(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = mem_dup(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key_type = BR_KEYTYPE_EC;
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = mem_dup(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    } else {
        free(dn.data);
        return -1;
    }

    return 0;
}

/* Parse PEM bundle into trust anchors */
static size_t load_pem_anchors(const char* filepath, br_x509_trust_anchor** out_anchors) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 1048576) {
        fclose(fp);
        return 0;
    }

    char* pem_buf = malloc(sz + 1);
    if (!pem_buf) {
        fclose(fp);
        return 0;
    }
    size_t rd = fread(pem_buf, 1, sz, fp);
    fclose(fp);
    pem_buf[rd] = '\0';

    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);

    size_t ta_cap = 16;
    size_t ta_cnt = 0;
    br_x509_trust_anchor* tas = malloc(sizeof(br_x509_trust_anchor) * ta_cap);

    buf_t der = {0};
    br_pem_decoder_setdest(&pc, buf_append, &der);

    size_t pos = 0;
    while (pos < rd) {
        size_t pushed = br_pem_decoder_push(&pc, pem_buf + pos, rd - pos);
        pos += pushed;

        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            der.len = 0;
        } else if (ev == BR_PEM_END_OBJ) {
            const char* name = br_pem_decoder_name(&pc);
            if (name && strstr(name, "CERTIFICATE") && der.len > 0) {
                if (ta_cnt >= ta_cap) {
                    ta_cap *= 2;
                    tas = realloc(tas, sizeof(br_x509_trust_anchor) * ta_cap);
                }
                if (parse_cert_to_anchor(der.data, der.len, &tas[ta_cnt]) == 0) {
                    ta_cnt++;
                }
            }
            der.len = 0;
        } else if (ev == BR_PEM_ERROR) {
            break;
        }
    }

    free(der.data);
    free(pem_buf);

    *out_anchors = tas;
    return ta_cnt;
}

static void free_anchor_contents(br_x509_trust_anchor* ta) {
    if (!ta) return;
    free(ta->dn.data);
    if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
        free(ta->pkey.key.rsa.n);
        free(ta->pkey.key.rsa.e);
    } else if (ta->pkey.key_type == BR_KEYTYPE_EC) {
        free(ta->pkey.key.ec.q);
    }
}

tls_client_t* tls_client_new(const char* ca_anchors_file) {
    tls_client_t* ctx = calloc(1, sizeof(tls_client_t));
    if (!ctx) return NULL;
    ctx->fd = -1;

    if (ca_anchors_file) {
        ctx->num_anchors = load_pem_anchors(ca_anchors_file, &ctx->anchors);
    }
    return ctx;
}

int tls_client_connect(tls_client_t* ctx, int fd, const char* sni_hostname) {
    if (!ctx || fd < 0) return -1;
    ctx->fd = fd;

    /* Fail closed if system RTC clock is before build epoch */
    time_t now = time(NULL);
    if (now < MIN_BUILD_EPOCH) {
        ctx->last_err = BR_ERR_X509_TIME_UNKNOWN;
        return -1;
    }

    /* Initialize full client profile */
    br_ssl_client_init_full(&ctx->sc, &ctx->xc, ctx->anchors, ctx->num_anchors);

    /* Enforce TLS 1.2 minimum */
    br_ssl_engine_set_versions(&ctx->sc.eng, BR_TLS12, BR_TLS12);

    /* Set certificate validity time from RTC */
    uint32_t days = (uint32_t)(now / 86400) + 719528;
    uint32_t seconds = (uint32_t)(now % 86400);
    br_x509_minimal_set_time(&ctx->xc, days, seconds);

    /* Inject entropy */
    unsigned char seed[32];
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd >= 0) {
        read(rfd, seed, sizeof(seed));
        close(rfd);
    } else {
        rfd = open("/var/lib/random-seed", O_RDONLY);
        if (rfd >= 0) {
            read(rfd, seed, sizeof(seed));
            close(rfd);
        } else {
            for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 37 + 13);
        }
    }
    br_ssl_engine_inject_entropy(&ctx->sc.eng, seed, sizeof(seed));

    /* Setup bi-directional I/O buffer */
    br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf, sizeof(ctx->iobuf), 1);

    /* Reset client engine with SNI hostname */
    br_ssl_client_reset(&ctx->sc, sni_hostname ? sni_hostname : "", 0);

    /* Initialize BearSSL simplified I/O context */
    br_sslio_init(&ctx->ioc, &ctx->sc.eng, sock_read, &ctx->fd, sock_write, &ctx->fd);

    return 0;
}

ssize_t tls_client_read(tls_client_t* ctx, void* buf, size_t len) {
    if (!ctx || ctx->fd < 0 || !buf || len == 0) return -1;
    int r = br_sslio_read(&ctx->ioc, buf, len);
    if (r < 0) {
        ctx->last_err = br_ssl_engine_last_error(&ctx->sc.eng);
        return -1;
    }
    return (ssize_t)r;
}

ssize_t tls_client_write(tls_client_t* ctx, const void* buf, size_t len) {
    if (!ctx || ctx->fd < 0 || !buf || len == 0) return -1;
    if (br_sslio_write_all(&ctx->ioc, buf, len) < 0) {
        ctx->last_err = br_ssl_engine_last_error(&ctx->sc.eng);
        return -1;
    }
    if (br_sslio_flush(&ctx->ioc) < 0) {
        ctx->last_err = br_ssl_engine_last_error(&ctx->sc.eng);
        return -1;
    }
    return (ssize_t)len;
}

int tls_client_flush(tls_client_t* ctx) {
    if (!ctx || ctx->fd < 0) return -1;
    return br_sslio_flush(&ctx->ioc);
}

void tls_client_close(tls_client_t* ctx) {
    if (!ctx || ctx->fd < 0) return;
    br_sslio_close(&ctx->ioc);
}

void tls_client_free(tls_client_t* ctx) {
    if (!ctx) return;
    if (ctx->anchors) {
        for (size_t i = 0; i < ctx->num_anchors; i++) {
            free_anchor_contents(&ctx->anchors[i]);
        }
        free(ctx->anchors);
    }
    free(ctx);
}

int tls_client_last_error(tls_client_t* ctx) {
    return ctx ? ctx->last_err : -1;
}

const char* tls_client_error_string(int err) {
    switch (err) {
        case BR_ERR_BAD_PARAM: return "bad parameter";
        case BR_ERR_BAD_STATE: return "bad state";
        case BR_ERR_UNSUPPORTED_VERSION: return "unsupported SSL/TLS version";
        case BR_ERR_BAD_VERSION: return "bad SSL/TLS version";
        case BR_ERR_BAD_LENGTH: return "bad record length";
        case BR_ERR_TOO_LARGE: return "record too large";
        case BR_ERR_BAD_MAC: return "bad record MAC";
        case BR_ERR_NO_RANDOM: return "no random source";
        case BR_ERR_UNKNOWN_TYPE: return "unknown record type";
        case BR_ERR_UNEXPECTED: return "unexpected message";
        case BR_ERR_BAD_CCS: return "bad change cipher spec";
        case BR_ERR_BAD_ALERT: return "bad alert";
        case BR_ERR_BAD_HANDSHAKE: return "bad handshake";
        case BR_ERR_OVERSIZED_ID: return "oversized session ID";
        case BR_ERR_BAD_CIPHER_SUITE: return "bad cipher suite";
        case BR_ERR_BAD_COMPRESSION: return "bad compression";
        case BR_ERR_BAD_FRAGLEN: return "bad fragment length";
        case BR_ERR_BAD_SECRENEG: return "bad secure renegotiation";
        case BR_ERR_EXTRA_EXTENSION: return "extra extension";
        case BR_ERR_BAD_SNI: return "bad SNI";
        case BR_ERR_BAD_HELLO_DONE: return "bad ServerHelloDone";
        case BR_ERR_LIMIT_EXCEEDED: return "limit exceeded";
        case BR_ERR_BAD_FINISHED: return "bad finished";
        case BR_ERR_RESUME_MISMATCH: return "resume mismatch";
        case BR_ERR_INVALID_ALGORITHM: return "invalid algorithm";
        case BR_ERR_BAD_SIGNATURE: return "bad signature";
        case BR_ERR_WRONG_KEY_USAGE: return "wrong key usage";
        case BR_ERR_NO_CLIENT_AUTH: return "no client auth";
        case BR_ERR_IO: return "I/O error";
        case BR_ERR_X509_INVALID_VALUE: return "X.509 invalid value";
        case BR_ERR_X509_TRUNCATED: return "X.509 truncated certificate";
        case BR_ERR_X509_EMPTY_CHAIN: return "X.509 empty certificate chain";
        case BR_ERR_X509_INNER_TRUNC: return "X.509 inner truncated";
        case BR_ERR_X509_BAD_TAG_CLASS: return "X.509 bad tag class";
        case BR_ERR_X509_BAD_TAG_VALUE: return "X.509 bad tag value";
        case BR_ERR_X509_INDEFINITE_LENGTH: return "X.509 indefinite length";
        case BR_ERR_X509_EXTRA_ELEMENT: return "X.509 extra element";
        case BR_ERR_X509_UNEXPECTED: return "X.509 unexpected ASN.1 element";
        case BR_ERR_X509_NOT_CONSTRUCTED: return "X.509 not constructed";
        case BR_ERR_X509_NOT_PRIMITIVE: return "X.509 not primitive";
        case BR_ERR_X509_PARTIAL_BYTE: return "X.509 partial byte";
        case BR_ERR_X509_BAD_BOOLEAN: return "X.509 bad boolean";
        case BR_ERR_X509_OVERFLOW: return "X.509 overflow";
        case BR_ERR_X509_BAD_DN: return "X.509 bad distinguished name";
        case BR_ERR_X509_BAD_TIME: return "X.509 bad time";
        case BR_ERR_X509_UNSUPPORTED: return "X.509 unsupported algorithm";
        case BR_ERR_X509_LIMIT_EXCEEDED: return "X.509 limit exceeded";
        case BR_ERR_X509_WRONG_KEY_TYPE: return "X.509 wrong key type";
        case BR_ERR_X509_TIME_UNKNOWN: return "X.509 time unknown";
        case BR_ERR_X509_NOT_TRUSTED: return "X.509 certificate not trusted";
        case BR_ERR_X509_EXPIRED: return "X.509 certificate expired";
        case BR_ERR_X509_DN_MISMATCH: return "X.509 DN mismatch";
        case BR_ERR_X509_BAD_SERVER_NAME: return "X.509 server name mismatch";
        case BR_ERR_X509_CRITICAL_EXTENSION: return "X.509 unsupported critical extension";
        case BR_ERR_X509_NOT_CA: return "X.509 not a CA";
        case BR_ERR_X509_FORBIDDEN_KEY_USAGE: return "X.509 forbidden key usage";
        case BR_ERR_X509_WEAK_PUBLIC_KEY: return "X.509 weak public key";
        default: return "unknown TLS error";
    }
}
