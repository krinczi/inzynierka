#ifndef INC_MBEDTLS_PLATFORM_RNG_H_
#define INC_MBEDTLS_PLATFORM_RNG_H_

#include <stddef.h>

int MbedtlsPlatform_Random(void *context,
                           unsigned char *output,
                           size_t length);

#endif /* INC_MBEDTLS_PLATFORM_RNG_H_ */
