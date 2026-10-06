// mbedTLS 3.6 configuration: a lean TLS 1.2 client for the HTTP client.
#pragma once

#include <limits.h>

// Platform
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT   // pico_mbedtls: RP2350 TRNG
#define MBEDTLS_ALLOW_PRIVATE_ACCESS   // certificate validity dates in the verify callback
// No MBEDTLS_HAVE_TIME(_DATE): certificate dates are checked against the
// GNSS UTC clock in our verify callback instead.

// TLS 1.2 client
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
// Servers may send full 16 KB records unless they honour max_fragment_length.
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

// Key exchange: ECDHE with ECDSA or RSA certificates
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_BIGNUM_C

// Ciphers: AES-GCM, ChaCha20-Poly1305
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

// Hashes (software: the SHA-256 engine is used by the downloads themselves)
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C  // legacy signatures some chains still carry

// Randomness
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// Certificates
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

#define MBEDTLS_ERROR_C
