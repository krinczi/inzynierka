#ifndef INC_MBEDTLS_COMPAT_H_
#define INC_MBEDTLS_COMPAT_H_

#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"

static inline int MbedtlsCompat_HandshakeOver(mbedtls_ssl_context *context)
{
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_ssl_is_handshake_over(context);
#else
    return context->state == MBEDTLS_SSL_HANDSHAKE_OVER;
#endif
}

static inline int MbedtlsCompat_Sha256Starts(mbedtls_sha256_context *context,
                                             int is224)
{
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_sha256_starts(context, is224);
#else
    return mbedtls_sha256_starts_ret(context, is224);
#endif
}

static inline int MbedtlsCompat_Sha256Update(mbedtls_sha256_context *context,
                                             const unsigned char *input,
                                             size_t length)
{
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_sha256_update(context, input, length);
#else
    return mbedtls_sha256_update_ret(context, input, length);
#endif
}

static inline int MbedtlsCompat_Sha256Finish(mbedtls_sha256_context *context,
                                             unsigned char output[32])
{
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_sha256_finish(context, output);
#else
    return mbedtls_sha256_finish_ret(context, output);
#endif
}

#endif /* INC_MBEDTLS_COMPAT_H_ */
