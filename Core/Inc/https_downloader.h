#ifndef INC_HTTPS_DOWNLOADER_H_
#define INC_HTTPS_DOWNLOADER_H_

#include <stdint.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

#include "flash_staging.h"

/*
 * Maximum complete HTTPS URL accepted by the Jobs download path. Keep this
 * public so the Jobs validator cannot accept a URL that the downloader rejects.
 */
#define HTTPS_DOWNLOADER_URL_MAX_LENGTH  2048U

typedef enum
{
    HTTPS_DOWNLOADER_IDLE = 0,
    HTTPS_DOWNLOADER_RUNNING,
    HTTPS_DOWNLOADER_SUCCEEDED,
    HTTPS_DOWNLOADER_FAILED
} HttpsDownloaderStatus;

/*
 * Reuses the already initialized trust anchor and random generator owned by
 * the AWS MQTT TLS session. The downloader creates a separate TLS connection.
 */
void HttpsDownloader_Init(mbedtls_x509_crt *root_ca,
                          mbedtls_ctr_drbg_context *rng);

/* Copies all arguments. The URL is deliberately never exposed through logs. */
int HttpsDownloader_Start(
    const char *url,
    uint32_t expected_size,
    const uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE]);

/*
 * Advances at most one bounded network/TLS/Flash step. Returns 1 after a
 * verified file is committed, -1 after a terminal failure, or 0 otherwise.
 */
int HttpsDownloader_Process(void);

void HttpsDownloader_Abort(void);
void HttpsDownloader_Reset(void);

HttpsDownloaderStatus HttpsDownloader_GetStatus(void);
uint32_t HttpsDownloader_GetBytesReceived(void);
uint32_t HttpsDownloader_GetExpectedSize(void);
const char *HttpsDownloader_GetLastError(void);

#endif /* INC_HTTPS_DOWNLOADER_H_ */
