#ifndef WIFI_SHELL_MBEDTLS_LIBPEER_USER_CONFIG_H
#define WIFI_SHELL_MBEDTLS_LIBPEER_USER_CONFIG_H

/* Zephyr's mbedTLS Kconfig does not expose this option, but libpeer's
 * DTLS-SRTP path requires the RFC 5764 API from mbedTLS.
 */
#define MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_TIMING_C
#define MBEDTLS_TIMING_ALT

#endif /* WIFI_SHELL_MBEDTLS_LIBPEER_USER_CONFIG_H */
