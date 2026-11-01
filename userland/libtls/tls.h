#ifndef _MYOS_LIBTLS_H_
#define _MYOS_LIBTLS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tls_client tls_client_t;

/* Create a TLS client context, optionally loading root anchors from ca_file */
tls_client_t* tls_client_new(const char* ca_anchors_file);

/* Set SNI and initiate TLS handshake over connected socket fd */
int tls_client_connect(tls_client_t* ctx, int fd, const char* sni_hostname);

/* Read decrypted data from TLS connection */
ssize_t tls_client_read(tls_client_t* ctx, void* buf, size_t len);

/* Write data to be encrypted and sent over TLS connection */
ssize_t tls_client_write(tls_client_t* ctx, const void* buf, size_t len);

/* Flush buffered TLS records */
int tls_client_flush(tls_client_t* ctx);

/* Close TLS connection cleanly */
void tls_client_close(tls_client_t* ctx);

/* Free TLS client context and loaded trust anchors */
void tls_client_free(tls_client_t* ctx);

/* Get last error code or error message */
int tls_client_last_error(tls_client_t* ctx);
const char* tls_client_error_string(int err);

#ifdef __cplusplus
}
#endif

#endif /* _MYOS_LIBTLS_H_ */
