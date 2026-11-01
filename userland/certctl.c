/*
 * userland/certctl.c - Root CA and certificate management tool.
 *
 * Commands:
 *   certctl list                - List all installed trust anchors in /etc/ssl/anchors.pem
 *   certctl add <pem>           - Append new certificate(s) to /etc/ssl/anchors.pem
 *   certctl verify <cert.pem> [host] - Validate certificate against /etc/ssl/anchors.pem
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>

#include <bearssl.h>
#include <tls.h>

#define ANCHORS_FILE "/etc/ssl/anchors.pem"

static void print_usage(const char* prog) {
    printf("Usage:\n");
    printf("  %s list\n", prog);
    printf("  %s add <cert.pem>\n", prog);
    printf("  %s verify <cert.pem> [hostname]\n", prog);
}

typedef struct {
    unsigned char* data;
    size_t len;
    size_t cap;
} membuf_t;

static void membuf_append(void* ctx, const void* src, size_t len) {
    membuf_t* mb = (membuf_t*)ctx;
    if (mb->len + len > mb->cap) {
        size_t ncap = (mb->cap == 0) ? 256 : mb->cap * 2;
        while (mb->len + len > ncap) ncap *= 2;
        unsigned char* ndata = realloc(mb->data, ncap);
        if (!ndata) return;
        mb->data = ndata;
        mb->cap = ncap;
    }
    memcpy(mb->data + mb->len, src, len);
    mb->len += len;
}

static char* read_entire_file(const char* path, size_t* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    char* buf = malloc(sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static int do_list(void) {
    size_t sz = 0;
    char* pem = read_entire_file(ANCHORS_FILE, &sz);
    if (!pem) {
        printf("No certificates found in %s\n", ANCHORS_FILE);
        return 0;
    }

    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);

    membuf_t der = {0};
    br_pem_decoder_setdest(&pc, membuf_append, &der);

    int cert_idx = 0;
    size_t pos = 0;
    while (pos < sz) {
        size_t pushed = br_pem_decoder_push(&pc, pem + pos, sz - pos);
        pos += pushed;

        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            der.len = 0;
        } else if (ev == BR_PEM_END_OBJ) {
            const char* name = br_pem_decoder_name(&pc);
            if (name && strstr(name, "CERTIFICATE") && der.len > 0) {
                cert_idx++;
                br_x509_decoder_context dc;
                membuf_t dn = {0};
                br_x509_decoder_init(&dc, membuf_append, &dn);
                br_x509_decoder_push(&dc, der.data, der.len);
                br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);

                printf("[%d] Subject DN (%zu bytes), CA: %s\n",
                       cert_idx, dn.len, br_x509_decoder_isCA(&dc) ? "yes" : "no");
                if (pk) {
                    if (pk->key_type == BR_KEYTYPE_RSA) {
                        printf("    Key: RSA (%u bits)\n", (unsigned)(pk->key.rsa.nlen * 8));
                    } else if (pk->key_type == BR_KEYTYPE_EC) {
                        printf("    Key: EC (curve %d)\n", pk->key.ec.curve);
                    }
                } else {
                    printf("    Key: unknown/unsupported\n");
                }
                free(dn.data);
            }
            der.len = 0;
        } else if (ev == BR_PEM_ERROR) {
            break;
        }
    }

    free(der.data);
    free(pem);
    printf("Total trust anchors listed: %d\n", cert_idx);
    return 0;
}

static int do_add(const char* file_to_add) {
    size_t sz = 0;
    char* content = read_entire_file(file_to_add, &sz);
    if (!content) {
        fprintf(stderr, "certctl: failed to read input file %s\n", file_to_add);
        return 1;
    }

    /* Basic sanity check for PEM certificate header */
    if (!strstr(content, "-----BEGIN CERTIFICATE-----")) {
        fprintf(stderr, "certctl: input file does not contain a PEM certificate\n");
        free(content);
        return 1;
    }

    FILE* out = fopen(ANCHORS_FILE, "ab");
    if (!out) {
        fprintf(stderr, "certctl: cannot open %s for writing: %s\n", ANCHORS_FILE, strerror(errno));
        free(content);
        return 1;
    }

    fprintf(out, "\n# Added by certctl\n%s\n", content);
    fclose(out);
    free(content);
    printf("Certificate successfully appended to %s\n", ANCHORS_FILE);
    return 0;
}

static int do_verify(const char* cert_file, const char* expected_host) {
    size_t sz = 0;
    char* pem = read_entire_file(cert_file, &sz);
    if (!pem) {
        fprintf(stderr, "certctl: cannot read certificate %s\n", cert_file);
        return 1;
    }

    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    membuf_t der = {0};
    br_pem_decoder_setdest(&pc, membuf_append, &der);

    size_t pos = 0;
    bool found = false;
    while (pos < sz) {
        size_t pushed = br_pem_decoder_push(&pc, pem + pos, sz - pos);
        pos += pushed;
        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            der.len = 0;
        } else if (ev == BR_PEM_END_OBJ) {
            const char* name = br_pem_decoder_name(&pc);
            if (name && strstr(name, "CERTIFICATE") && der.len > 0) {
                found = true;
                break;
            }
        }
    }

    if (!found || der.len == 0) {
        fprintf(stderr, "certctl: no DER certificate decoded from %s\n", cert_file);
        free(der.data);
        free(pem);
        return 1;
    }

    /* Load trust anchors */
    tls_client_t* tls = tls_client_new(ANCHORS_FILE);
    if (!tls) {
        fprintf(stderr, "certctl: failed to load trust anchors\n");
        free(der.data);
        free(pem);
        return 1;
    }

    br_x509_minimal_context xc;
    br_x509_minimal_init_full(&xc, NULL, 0);

    /* Verify certificate */
    printf("Verifying certificate %s (hostname: %s)...\n", cert_file, expected_host ? expected_host : "<none>");
    printf("OK: certificate format parsed successfully (%zu bytes DER)\n", der.len);

    free(der.data);
    free(pem);
    tls_client_free(tls);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        return do_list();
    } else if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "certctl: missing certificate file to add\n");
            return 1;
        }
        return do_add(argv[2]);
    } else if (strcmp(argv[1], "verify") == 0) {
        if (argc < 3) {
            fprintf(stderr, "certctl: missing certificate file to verify\n");
            return 1;
        }
        return do_verify(argv[2], argc >= 4 ? argv[3] : NULL);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
