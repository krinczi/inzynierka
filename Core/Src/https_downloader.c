#include "https_downloader.h"
#include "mbedtls_compat.h"

#include "debug_logger.h"
#include "main.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef ECONNRESET
#define ECONNRESET 104
#endif

#define HTTPS_URL_MAX_LENGTH             HTTPS_DOWNLOADER_URL_MAX_LENGTH
#define HTTPS_HOST_MAX_LENGTH             253U
#define HTTPS_TARGET_MAX_LENGTH          HTTPS_DOWNLOADER_URL_MAX_LENGTH
#define HTTPS_REQUEST_FIXED_OVERHEAD      192U
#define HTTPS_REQUEST_MAX_LENGTH \
    (HTTPS_TARGET_MAX_LENGTH + HTTPS_HOST_MAX_LENGTH + \
     HTTPS_REQUEST_FIXED_OVERHEAD)
#define HTTPS_HEADER_MAX_LENGTH          2048U
#define HTTPS_IO_BUFFER_SIZE             1024U
#define HTTPS_FLASH_SERVICE_CHUNK_SIZE    256U
#define HTTPS_PORT                        443U
#define HTTPS_DNS_TIMEOUT_MS            15000U
#define HTTPS_TCP_TIMEOUT_MS            10000U
#define HTTPS_TLS_TIMEOUT_MS            60000U
#define HTTPS_IDLE_TIMEOUT_MS           15000U
#define HTTPS_TOTAL_TIMEOUT_MS         480000U
#define HTTPS_RETRY_BASE_MS              5000U
#define HTTPS_MAX_ATTEMPTS                  3U
#define HTTPS_PROGRESS_STEP_BYTES       65536U

typedef enum
{
    DOWNLOADER_STATE_IDLE = 0,
    DOWNLOADER_STATE_RETRY_WAIT,
    DOWNLOADER_STATE_DNS_START,
    DOWNLOADER_STATE_DNS_WAIT,
    DOWNLOADER_STATE_TCP_START,
    DOWNLOADER_STATE_TCP_WAIT,
    DOWNLOADER_STATE_TLS_SETUP,
    DOWNLOADER_STATE_TLS_HANDSHAKE,
    DOWNLOADER_STATE_SEND_REQUEST,
    DOWNLOADER_STATE_FLASH_PREPARE,
    DOWNLOADER_STATE_READ_RESPONSE,
    DOWNLOADER_STATE_SUCCEEDED,
    DOWNLOADER_STATE_FAILED
} DownloaderState;

typedef struct
{
    DownloaderState state;
    HttpsDownloaderStatus status;
    mbedtls_x509_crt *root_ca;
    mbedtls_ctr_drbg_context *rng;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    uint8_t tls_initialized;
    uint8_t flash_started;
    uint8_t attempt;
    uint8_t dns_generation;
    volatile uint8_t dns_start_done;
    volatile uint8_t dns_done;
    volatile err_t dns_result;
    ip_addr_t dns_address;
    uint32_t state_deadline_ms;
    uint32_t retry_after_ms;
    uint32_t total_deadline_ms;
    uint32_t expected_size;
    uint32_t bytes_received;
    uint32_t next_progress_bytes;
    uint8_t expected_sha256[FLASH_STAGING_SHA256_SIZE];
    FlashStagingRequest staging_request;
    char host[HTTPS_HOST_MAX_LENGTH + 1U];
    char target[HTTPS_TARGET_MAX_LENGTH + 1U];
    char resolved_ip[48];
    char request[HTTPS_REQUEST_MAX_LENGTH];
    size_t request_length;
    size_t request_sent;
    uint8_t io_buffer[HTTPS_IO_BUFFER_SIZE];
    uint8_t header_buffer[HTTPS_HEADER_MAX_LENGTH];
    size_t header_length;
    size_t pending_body_offset;
    size_t pending_body_length;
    uint8_t headers_complete;
    char last_error[48];
} HttpsDownloaderContext;

static HttpsDownloaderContext downloader;
static int downloader_initialized;

static void Downloader_Log(const char *message)
{
    Debug_LogLine(DEBUG_SRC_TLS, message);
}

static void Downloader_Logf(const char *format, ...)
{
    char message[160];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Debug_LogLine(DEBUG_SRC_TLS, message);
}

static int Downloader_DeadlineReached(uint32_t deadline_ms)
{
    return (int32_t)(HAL_GetTick() - deadline_ms) >= 0;
}

static void Downloader_InvalidateDns(void)
{
    downloader.dns_generation++;
    if (downloader.dns_generation == 0U)
    {
        downloader.dns_generation = 1U;
    }
    downloader.dns_done = 0U;
}

static void Downloader_SetError(const char *reason)
{
    (void)snprintf(downloader.last_error,
                   sizeof(downloader.last_error),
                   "%s",
                   reason != NULL ? reason : "download_failed");
}

static void Downloader_FreeTls(void)
{
    if (downloader.tls_initialized != 0U)
    {
        mbedtls_ssl_free(&downloader.ssl);
        mbedtls_ssl_config_free(&downloader.config);
        downloader.tls_initialized = 0U;
    }

    if (downloader.net.fd >= 0)
    {
        (void)lwip_close(downloader.net.fd);
        downloader.net.fd = -1;
    }
}

static void Downloader_ResetAttemptBuffers(void)
{
    Downloader_FreeTls();
    mbedtls_net_init(&downloader.net);
    downloader.request_length = 0U;
    downloader.request_sent = 0U;
    downloader.header_length = 0U;
    downloader.pending_body_offset = 0U;
    downloader.pending_body_length = 0U;
    downloader.headers_complete = 0U;
    downloader.bytes_received = 0U;
    downloader.next_progress_bytes = HTTPS_PROGRESS_STEP_BYTES;
    downloader.dns_start_done = 0U;
    downloader.dns_done = 0U;
    downloader.dns_result = ERR_INPROGRESS;
    downloader.resolved_ip[0] = '\0';
}

static void Downloader_Fail(const char *reason, int retryable)
{
    uint32_t delay_ms;

    Downloader_SetError(reason);

    if (downloader.flash_started != 0U)
    {
        if (FlashStaging_Abort() != FLASH_STAGING_OK)
        {
            retryable = 0;
            Downloader_SetError("flash_abort_failed");
        }
        downloader.flash_started = 0U;
    }

    Downloader_FreeTls();

    if (retryable && (downloader.attempt < HTTPS_MAX_ATTEMPTS) &&
        !Downloader_DeadlineReached(downloader.total_deadline_ms))
    {
        delay_ms = HTTPS_RETRY_BASE_MS << (downloader.attempt - 1U);
        downloader.attempt++;
        downloader.retry_after_ms = HAL_GetTick() + delay_ms;
        downloader.state = DOWNLOADER_STATE_RETRY_WAIT;
        downloader.status = HTTPS_DOWNLOADER_RUNNING;
        Downloader_Logf("HTTPS: retry attempt=%u/%u in %lu ms reason=%s",
                        (unsigned int)downloader.attempt,
                        (unsigned int)HTTPS_MAX_ATTEMPTS,
                        (unsigned long)delay_ms,
                        downloader.last_error);
        return;
    }

    downloader.state = DOWNLOADER_STATE_FAILED;
    downloader.status = HTTPS_DOWNLOADER_FAILED;
    Downloader_Logf("HTTPS: FAILED reason=%s received=%lu/%lu",
                    downloader.last_error,
                    (unsigned long)downloader.bytes_received,
                    (unsigned long)downloader.expected_size);
}

static int Downloader_IsHostnameCharacter(char value)
{
    return ((value >= 'a') && (value <= 'z')) ||
           ((value >= 'A') && (value <= 'Z')) ||
           ((value >= '0') && (value <= '9')) ||
           (value == '.') || (value == '-');
}

static int Downloader_ParseUrl(const char *url)
{
    const char *authority;
    const char *authority_end;
    const char *target;
    const char *colon;
    const char *at_sign;
    size_t authority_length;
    size_t host_length;
    size_t target_length;
    size_t index;

    if ((url == NULL) || (strncmp(url, "https://", 8U) != 0) ||
        (strlen(url) > HTTPS_URL_MAX_LENGTH) || (strchr(url, '#') != NULL))
    {
        return -1;
    }

    authority = url + 8U;
    authority_end = authority;
    while ((*authority_end != '\0') && (*authority_end != '/') &&
           (*authority_end != '?'))
    {
        authority_end++;
    }

    authority_length = (size_t)(authority_end - authority);
    at_sign = memchr(authority, '@', authority_length);
    if ((authority_length == 0U) || (at_sign != NULL))
    {
        return -1;
    }

    colon = memchr(authority, ':', authority_length);
    if (colon != NULL)
    {
        if (((size_t)(authority_end - colon) != 4U) ||
            (memcmp(colon, ":443", 4U) != 0))
        {
            return -1;
        }
        host_length = (size_t)(colon - authority);
    }
    else
    {
        host_length = authority_length;
    }

    if ((host_length == 0U) || (host_length > HTTPS_HOST_MAX_LENGTH) ||
        (authority[0] == '.') || (authority[host_length - 1U] == '.') ||
        (authority[0] == '-') || (authority[host_length - 1U] == '-'))
    {
        return -1;
    }

    for (index = 0U; index < host_length; index++)
    {
        if (!Downloader_IsHostnameCharacter(authority[index]))
        {
            return -1;
        }
    }

    memcpy(downloader.host, authority, host_length);
    downloader.host[host_length] = '\0';

    target = authority_end;
    if (*target == '\0')
    {
        (void)snprintf(downloader.target, sizeof(downloader.target), "/");
        return 0;
    }

    target_length = strlen(target);
    if (*target == '?')
    {
        if ((target_length + 1U) > HTTPS_TARGET_MAX_LENGTH)
        {
            return -1;
        }
        downloader.target[0] = '/';
        memcpy(&downloader.target[1], target, target_length + 1U);
    }
    else
    {
        if (target_length > HTTPS_TARGET_MAX_LENGTH)
        {
            return -1;
        }
        memcpy(downloader.target, target, target_length + 1U);
    }

    for (index = 0U; downloader.target[index] != '\0'; index++)
    {
        unsigned char value = (unsigned char)downloader.target[index];
        if ((value <= 0x20U) || (value >= 0x7FU))
        {
            return -1;
        }
    }

    return 0;
}

static void Downloader_DnsFound(const char *name,
                                const ip_addr_t *ipaddr,
                                void *arg)
{
    uint8_t generation = (uint8_t)(uintptr_t)arg;
    (void)name;

    if (generation != downloader.dns_generation)
    {
        return;
    }

    if ((ipaddr != NULL) && IP_IS_V4(ipaddr))
    {
        downloader.dns_address = *ipaddr;
        downloader.dns_result = ERR_OK;
    }
    else
    {
        downloader.dns_result = ERR_VAL;
    }
    downloader.dns_done = 1U;
}

static void Downloader_DnsStartOnTcpipThread(void *arg)
{
    uint8_t generation = (uint8_t)(uintptr_t)arg;
    err_t result;

    if (generation != downloader.dns_generation)
    {
        return;
    }

    result = dns_gethostbyname(downloader.host,
                               &downloader.dns_address,
                               Downloader_DnsFound,
                               (void *)(uintptr_t)generation);
    downloader.dns_result = result;
    downloader.dns_start_done = 1U;

    if (result == ERR_OK)
    {
        downloader.dns_done = 1U;
    }
    else if (result != ERR_INPROGRESS)
    {
        downloader.dns_done = 1U;
    }
}

static void Downloader_StartDns(void)
{
    err_t result;

    downloader.dns_generation++;
    if (downloader.dns_generation == 0U)
    {
        downloader.dns_generation = 1U;
    }
    downloader.dns_start_done = 0U;
    downloader.dns_done = 0U;
    downloader.dns_result = ERR_INPROGRESS;
    downloader.state_deadline_ms = HAL_GetTick() + HTTPS_DNS_TIMEOUT_MS;

    result = tcpip_callback(Downloader_DnsStartOnTcpipThread,
                            (void *)(uintptr_t)downloader.dns_generation);
    if (result != ERR_OK)
    {
        Downloader_Fail("dns_start_failed", 1);
        return;
    }

    downloader.state = DOWNLOADER_STATE_DNS_WAIT;
    Downloader_Logf("HTTPS: resolving host attempt=%u/%u",
                    (unsigned int)downloader.attempt,
                    (unsigned int)HTTPS_MAX_ATTEMPTS);
}

static int Downloader_TcpStart(void)
{
    struct sockaddr_in address;
    int nonblocking = 1;
    int result;
    struct timeval timeout;

    downloader.net.fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (downloader.net.fd < 0)
    {
        Downloader_Fail("socket_failed", 1);
        return -1;
    }

    if (lwip_ioctl(downloader.net.fd, FIONBIO, &nonblocking) < 0)
    {
        Downloader_Fail("socket_nonblock_failed", 1);
        return -1;
    }

    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;
    (void)lwip_setsockopt(downloader.net.fd,
                          SOL_SOCKET,
                          SO_RCVTIMEO,
                          &timeout,
                          sizeof(timeout));
    (void)lwip_setsockopt(downloader.net.fd,
                          SOL_SOCKET,
                          SO_SNDTIMEO,
                          &timeout,
                          sizeof(timeout));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = PP_HTONS(HTTPS_PORT);
    address.sin_addr.s_addr = inet_addr(downloader.resolved_ip);

    errno = 0;
    result = lwip_connect(downloader.net.fd,
                          (struct sockaddr *)&address,
                          sizeof(address));
    if (result == 0)
    {
        downloader.state = DOWNLOADER_STATE_TLS_SETUP;
        Downloader_Log("HTTPS: TCP connected");
        return 0;
    }

    if ((errno != EINPROGRESS) && (errno != EALREADY) &&
        (errno != EWOULDBLOCK) && (errno != EAGAIN))
    {
        Downloader_Fail("tcp_connect_failed", 1);
        return -1;
    }

    downloader.state_deadline_ms = HAL_GetTick() + HTTPS_TCP_TIMEOUT_MS;
    downloader.state = DOWNLOADER_STATE_TCP_WAIT;
    return 0;
}

static void Downloader_TcpPoll(void)
{
    fd_set write_fds;
    struct timeval timeout;
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    int result;

    FD_ZERO(&write_fds);
    FD_SET(downloader.net.fd, &write_fds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    result = lwip_select(downloader.net.fd + 1,
                         NULL,
                         &write_fds,
                         NULL,
                         &timeout);

    if (result > 0 && FD_ISSET(downloader.net.fd, &write_fds))
    {
        result = lwip_getsockopt(downloader.net.fd,
                                 SOL_SOCKET,
                                 SO_ERROR,
                                 &socket_error,
                                 &error_length);
        if ((result == 0) && (socket_error == 0))
        {
            downloader.state = DOWNLOADER_STATE_TLS_SETUP;
            Downloader_Log("HTTPS: TCP connected");
            return;
        }

        if ((result != 0) ||
            ((socket_error != EINPROGRESS) && (socket_error != EALREADY) &&
             (socket_error != EWOULDBLOCK) && (socket_error != EAGAIN)))
        {
            Downloader_Fail("tcp_connect_failed", 1);
            return;
        }
    }
    else if (result < 0)
    {
        Downloader_Fail("tcp_select_failed", 1);
        return;
    }

    if (Downloader_DeadlineReached(downloader.state_deadline_ms))
    {
        Downloader_Fail("tcp_connect_timeout", 1);
    }
}

static int Downloader_TlsSend(void *context,
                              const unsigned char *buffer,
                              size_t length)
{
    int descriptor = ((mbedtls_net_context *)context)->fd;
    size_t bounded_length = length > HTTPS_IO_BUFFER_SIZE
                                ? HTTPS_IO_BUFFER_SIZE
                                : length;
    int result;

    errno = 0;
    result = lwip_send(descriptor, buffer, bounded_length, MSG_DONTWAIT);
    if (result > 0)
    {
        return result;
    }
    if (result == 0)
    {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINPROGRESS))
    {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int Downloader_TlsRecv(void *context,
                              unsigned char *buffer,
                              size_t length)
{
    int descriptor = ((mbedtls_net_context *)context)->fd;
    size_t bounded_length = length > HTTPS_IO_BUFFER_SIZE
                                ? HTTPS_IO_BUFFER_SIZE
                                : length;
    int result;

    errno = 0;
    result = lwip_recv(descriptor, buffer, bounded_length, MSG_DONTWAIT);
    if (result > 0)
    {
        return result;
    }
    if (result == 0)
    {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINPROGRESS))
    {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (errno == ECONNRESET)
    {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void Downloader_TlsSetup(void)
{
    int result;

    mbedtls_ssl_init(&downloader.ssl);
    mbedtls_ssl_config_init(&downloader.config);
    downloader.tls_initialized = 1U;

    result = mbedtls_ssl_config_defaults(&downloader.config,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0)
    {
        Downloader_Fail("tls_config_failed", 1);
        return;
    }

    mbedtls_ssl_conf_authmode(&downloader.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_min_version(&downloader.config,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&downloader.config,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_ca_chain(&downloader.config, downloader.root_ca, NULL);
    mbedtls_ssl_conf_rng(&downloader.config,
                         mbedtls_ctr_drbg_random,
                         downloader.rng);

    result = mbedtls_ssl_setup(&downloader.ssl, &downloader.config);
    if (result == 0)
    {
        result = mbedtls_ssl_set_hostname(&downloader.ssl, downloader.host);
    }
    if (result != 0)
    {
        Downloader_Fail("tls_setup_failed", 1);
        return;
    }

    mbedtls_ssl_set_bio(&downloader.ssl,
                        &downloader.net,
                        Downloader_TlsSend,
                        Downloader_TlsRecv,
                        NULL);
    downloader.state_deadline_ms = HAL_GetTick() + HTTPS_TLS_TIMEOUT_MS;
    downloader.state = DOWNLOADER_STATE_TLS_HANDSHAKE;
    Downloader_Log("HTTPS: TLS handshake started");
}

static void Downloader_TlsHandshake(void)
{
    int result = mbedtls_ssl_handshake_step(&downloader.ssl);

    if (result == 0)
    {
        if (MbedtlsCompat_HandshakeOver(&downloader.ssl))
        {
            if (mbedtls_ssl_get_verify_result(&downloader.ssl) != 0U)
            {
                Downloader_Fail("certificate_verify_failed", 0);
                return;
            }

            downloader.request_length = (size_t)snprintf(
                downloader.request,
                sizeof(downloader.request),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: stm32h753-iot/1\r\n"
                "Accept: application/octet-stream\r\n"
                "Accept-Encoding: identity\r\n"
                "Connection: close\r\n\r\n",
                downloader.target,
                downloader.host);
            if (downloader.request_length >= sizeof(downloader.request))
            {
                Downloader_Fail("http_request_too_large", 0);
                return;
            }

            downloader.request_sent = 0U;
            downloader.state_deadline_ms = HAL_GetTick() + HTTPS_IDLE_TIMEOUT_MS;
            downloader.state = DOWNLOADER_STATE_SEND_REQUEST;
            Downloader_Log("HTTPS: TLS certificate verified");
        }
        return;
    }

    if ((result != MBEDTLS_ERR_SSL_WANT_READ) &&
        (result != MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        Downloader_Fail("tls_handshake_failed", 1);
        return;
    }

    if (Downloader_DeadlineReached(downloader.state_deadline_ms))
    {
        Downloader_Fail("tls_handshake_timeout", 1);
    }
}

static void Downloader_SendRequest(void)
{
    int result = mbedtls_ssl_write(
        &downloader.ssl,
        (const unsigned char *)&downloader.request[downloader.request_sent],
        downloader.request_length - downloader.request_sent);

    if (result > 0)
    {
        downloader.request_sent += (size_t)result;
        downloader.state_deadline_ms = HAL_GetTick() + HTTPS_IDLE_TIMEOUT_MS;
        if (downloader.request_sent == downloader.request_length)
        {
            downloader.state = DOWNLOADER_STATE_READ_RESPONSE;
            Downloader_Log("HTTPS: request sent");
        }
        return;
    }

    if ((result != MBEDTLS_ERR_SSL_WANT_READ) &&
        (result != MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        Downloader_Fail("http_send_failed", 1);
        return;
    }

    if (Downloader_DeadlineReached(downloader.state_deadline_ms))
    {
        Downloader_Fail("http_send_timeout", 1);
    }
}

static int Downloader_NameEquals(const uint8_t *start,
                                 size_t length,
                                 const char *expected)
{
    size_t expected_length = strlen(expected);
    size_t index;

    if (length != expected_length)
    {
        return 0;
    }

    for (index = 0U; index < length; index++)
    {
        if (tolower((unsigned char)start[index]) !=
            tolower((unsigned char)expected[index]))
        {
            return 0;
        }
    }
    return 1;
}

static int Downloader_ValueEquals(const uint8_t *start,
                                  size_t length,
                                  const char *expected)
{
    while ((length > 0U) && ((*start == ' ') || (*start == '\t')))
    {
        start++;
        length--;
    }
    while ((length > 0U) &&
           ((start[length - 1U] == ' ') || (start[length - 1U] == '\t')))
    {
        length--;
    }
    return Downloader_NameEquals(start, length, expected);
}

static int Downloader_ParseContentLength(const uint8_t *start,
                                         size_t length,
                                         uint32_t *value)
{
    uint32_t parsed = 0U;
    size_t index = 0U;
    int digit_found = 0;

    while ((index < length) && ((start[index] == ' ') || (start[index] == '\t')))
    {
        index++;
    }

    while ((index < length) && (start[index] >= '0') && (start[index] <= '9'))
    {
        uint32_t digit = (uint32_t)(start[index] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U)
        {
            return -1;
        }
        parsed = parsed * 10U + digit;
        digit_found = 1;
        index++;
    }

    while ((index < length) && ((start[index] == ' ') || (start[index] == '\t')))
    {
        index++;
    }

    if (!digit_found || (index != length))
    {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int Downloader_FindCrlf(const uint8_t *buffer,
                               size_t start,
                               size_t limit,
                               size_t *line_end)
{
    size_t index;
    for (index = start; (index + 1U) < limit; index++)
    {
        if ((buffer[index] == '\r') && (buffer[index + 1U] == '\n'))
        {
            *line_end = index;
            return 0;
        }
    }
    return -1;
}

static int Downloader_ParseHeaders(size_t header_bytes)
{
    size_t line_end;
    size_t position;
    uint32_t content_length = 0U;
    int content_length_found = 0;

    if ((Downloader_FindCrlf(downloader.header_buffer,
                             0U,
                             header_bytes,
                             &line_end) != 0) ||
        (line_end < 12U) ||
        (memcmp(downloader.header_buffer, "HTTP/1.", 7U) != 0) ||
        ((downloader.header_buffer[7] != '0') &&
         (downloader.header_buffer[7] != '1')) ||
        (downloader.header_buffer[8] != ' ') ||
        (downloader.header_buffer[9] != '2') ||
        (downloader.header_buffer[10] != '0') ||
        (downloader.header_buffer[11] != '0'))
    {
        Downloader_Fail("http_status_not_200", 0);
        return -1;
    }

    position = line_end + 2U;
    while ((position + 2U) <= header_bytes)
    {
        size_t colon;
        size_t name_length;
        const uint8_t *value;
        size_t value_length;

        if (Downloader_FindCrlf(downloader.header_buffer,
                                position,
                                header_bytes,
                                &line_end) != 0)
        {
            Downloader_Fail("invalid_http_headers", 0);
            return -1;
        }

        if (line_end == position)
        {
            break;
        }
        if ((downloader.header_buffer[position] == ' ') ||
            (downloader.header_buffer[position] == '\t'))
        {
            Downloader_Fail("folded_http_header", 0);
            return -1;
        }

        colon = position;
        while ((colon < line_end) && (downloader.header_buffer[colon] != ':'))
        {
            colon++;
        }
        if (colon == line_end)
        {
            Downloader_Fail("invalid_http_header", 0);
            return -1;
        }

        name_length = colon - position;
        value = &downloader.header_buffer[colon + 1U];
        value_length = line_end - colon - 1U;

        if (Downloader_NameEquals(&downloader.header_buffer[position],
                                  name_length,
                                  "Content-Length"))
        {
            if (content_length_found ||
                (Downloader_ParseContentLength(value,
                                               value_length,
                                               &content_length) != 0))
            {
                Downloader_Fail("invalid_content_length", 0);
                return -1;
            }
            content_length_found = 1;
        }
        else if (Downloader_NameEquals(&downloader.header_buffer[position],
                                       name_length,
                                       "Transfer-Encoding"))
        {
            Downloader_Fail("transfer_encoding_unsupported", 0);
            return -1;
        }
        else if (Downloader_NameEquals(&downloader.header_buffer[position],
                                       name_length,
                                       "Content-Encoding") &&
                 !Downloader_ValueEquals(value, value_length, "identity"))
        {
            Downloader_Fail("content_encoding_unsupported", 0);
            return -1;
        }

        position = line_end + 2U;
    }

    if (!content_length_found || (content_length != downloader.expected_size))
    {
        Downloader_Fail("content_length_mismatch", 0);
        return -1;
    }

    if (FlashStaging_PrepareArtifact(&downloader.staging_request) !=
        FLASH_STAGING_IN_PROGRESS)
    {
        Downloader_Fail("flash_begin_failed", 0);
        return -1;
    }
    downloader.flash_started = 1U;
    downloader.state = DOWNLOADER_STATE_FLASH_PREPARE;
    Downloader_Logf("HTTPS: response validated, Flash preparation size=%lu",
                    (unsigned long)downloader.expected_size);
    return 0;
}

static int Downloader_CompleteSuccess(void)
{
    FlashStagingResult result = FlashStaging_Finalize();
    if (result != FLASH_STAGING_OK)
    {
        const char *error =
            result == FLASH_STAGING_HASH_MISMATCH
                ? "sha256_mismatch"
                : result == FLASH_STAGING_INTEGRITY_ERROR
                      ? "firmware_validation_failed"
                      : "flash_finalize_failed";
        Downloader_Fail(error,
                        result == FLASH_STAGING_HASH_MISMATCH);
        return downloader.status == HTTPS_DOWNLOADER_FAILED ? -1 : 0;
    }

    downloader.flash_started = 0U;
    Downloader_FreeTls();
    Downloader_SetError("none");
    downloader.state = DOWNLOADER_STATE_SUCCEEDED;
    downloader.status = HTTPS_DOWNLOADER_SUCCEEDED;
    Downloader_Logf("HTTPS: SUCCEEDED received=%lu sha256=verified",
                    (unsigned long)downloader.bytes_received);
    return 1;
}

static int Downloader_WriteBody(const uint8_t *data, size_t length)
{
    if ((length > downloader.expected_size) ||
        (downloader.bytes_received > downloader.expected_size - length))
    {
        Downloader_Fail("body_exceeds_expected_size", 0);
        return -1;
    }

    if ((length > 0U) &&
        (FlashStaging_Write(data, length) != FLASH_STAGING_OK))
    {
        Downloader_Fail("flash_write_failed", 0);
        return -1;
    }

    downloader.bytes_received += (uint32_t)length;
    while ((downloader.bytes_received >= downloader.next_progress_bytes) &&
           (downloader.next_progress_bytes < downloader.expected_size))
    {
        Downloader_Logf("HTTPS: progress=%lu/%lu",
                        (unsigned long)downloader.bytes_received,
                        (unsigned long)downloader.expected_size);
        downloader.next_progress_bytes += HTTPS_PROGRESS_STEP_BYTES;
    }

    if (downloader.bytes_received == downloader.expected_size)
    {
        return Downloader_CompleteSuccess();
    }
    return 0;
}

static int Downloader_PrepareFlash(void)
{
    FlashStagingResult result = FlashStaging_PrepareStep();
    int write_result = 0;

    if (result == FLASH_STAGING_IN_PROGRESS)
    {
        return 0;
    }
    if (result != FLASH_STAGING_OK)
    {
        Downloader_Fail("flash_begin_failed", 0);
        return -1;
    }

    downloader.headers_complete = 1U;
    downloader.state = DOWNLOADER_STATE_READ_RESPONSE;
    Downloader_Logf("HTTPS: response accepted size=%lu",
                    (unsigned long)downloader.expected_size);

    if (downloader.pending_body_length > 0U)
    {
        write_result = Downloader_WriteBody(
            &downloader.header_buffer[downloader.pending_body_offset],
            downloader.pending_body_length);
    }

    downloader.pending_body_offset = 0U;
    downloader.pending_body_length = 0U;
    downloader.header_length = 0U;
    return write_result;
}

static int Downloader_HandleReceived(const uint8_t *data, size_t length)
{
    size_t index;

    if (downloader.headers_complete != 0U)
    {
        return Downloader_WriteBody(data, length);
    }

    if (length > sizeof(downloader.header_buffer) - downloader.header_length)
    {
        Downloader_Fail("http_headers_too_large", 0);
        return -1;
    }

    memcpy(&downloader.header_buffer[downloader.header_length], data, length);
    downloader.header_length += length;

    for (index = 0U; (index + 3U) < downloader.header_length; index++)
    {
        if ((downloader.header_buffer[index] == '\r') &&
            (downloader.header_buffer[index + 1U] == '\n') &&
            (downloader.header_buffer[index + 2U] == '\r') &&
            (downloader.header_buffer[index + 3U] == '\n'))
        {
            size_t header_bytes = index + 4U;
            size_t body_bytes = downloader.header_length - header_bytes;

            if (Downloader_ParseHeaders(header_bytes) != 0)
            {
                return -1;
            }
            downloader.pending_body_offset = header_bytes;
            downloader.pending_body_length = body_bytes;
            return 0;
        }
    }

    return 0;
}

static int Downloader_ReadResponse(void)
{
    size_t read_length = HTTPS_FLASH_SERVICE_CHUNK_SIZE;
    int result = mbedtls_ssl_read(&downloader.ssl,
                                  downloader.io_buffer,
                                  read_length);

    if (result > 0)
    {
        downloader.state_deadline_ms = HAL_GetTick() + HTTPS_IDLE_TIMEOUT_MS;
        return Downloader_HandleReceived(downloader.io_buffer, (size_t)result);
    }

    if ((result == MBEDTLS_ERR_SSL_WANT_READ) ||
        (result == MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        if (Downloader_DeadlineReached(downloader.state_deadline_ms))
        {
            Downloader_Fail("http_read_timeout", 1);
        }
        return 0;
    }

    if ((result == 0) || (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) ||
        (result == MBEDTLS_ERR_NET_CONN_RESET))
    {
        Downloader_Fail("body_truncated", 1);
        return downloader.status == HTTPS_DOWNLOADER_FAILED ? -1 : 0;
    }

    Downloader_Fail("http_read_failed", 1);
    return downloader.status == HTTPS_DOWNLOADER_FAILED ? -1 : 0;
}

void HttpsDownloader_Init(mbedtls_x509_crt *root_ca,
                          mbedtls_ctr_drbg_context *rng)
{
    uint8_t dns_generation = 1U;

    if (downloader_initialized)
    {
        HttpsDownloader_Abort();
        dns_generation = downloader.dns_generation;
    }

    /* Preserve embedded mbedTLS contexts and buffers after releasing resources. */
    mbedtls_net_init(&downloader.net);
    downloader_initialized = 1;
    downloader.dns_generation = dns_generation;
    downloader.root_ca = root_ca;
    downloader.rng = rng;
    downloader.state = DOWNLOADER_STATE_IDLE;
    downloader.status = HTTPS_DOWNLOADER_IDLE;
    downloader.tls_initialized = 0U;
    downloader.flash_started = 0U;
    downloader.attempt = 0U;
    downloader.dns_start_done = 0U;
    downloader.dns_done = 0U;
    downloader.dns_result = ERR_INPROGRESS;
    ip_addr_set_zero(&downloader.dns_address);
    downloader.state_deadline_ms = 0U;
    downloader.retry_after_ms = 0U;
    downloader.total_deadline_ms = 0U;
    downloader.expected_size = 0U;
    downloader.bytes_received = 0U;
    downloader.next_progress_bytes = HTTPS_PROGRESS_STEP_BYTES;
    downloader.host[0] = '\0';
    downloader.target[0] = '\0';
    downloader.resolved_ip[0] = '\0';
    downloader.request_length = 0U;
    downloader.request_sent = 0U;
    downloader.header_length = 0U;
    downloader.pending_body_offset = 0U;
    downloader.pending_body_length = 0U;
    downloader.headers_complete = 0U;
    memset(downloader.expected_sha256, 0, sizeof(downloader.expected_sha256));
    Downloader_SetError("none");
}

int HttpsDownloader_Start(
    const char *url,
    const FlashStagingRequest *request)
{
    const FlashStagingInfo *staging = FlashStaging_GetInfo();

    if ((downloader.root_ca == NULL) || (downloader.rng == NULL) ||
        (url == NULL) || (request == NULL) ||
        (strlen(url) > HTTPS_URL_MAX_LENGTH) ||
        (request->file_size_bytes == 0U) ||
        (request->file_size_bytes > staging->capacity_bytes) ||
        (downloader.status == HTTPS_DOWNLOADER_RUNNING))
    {
        return -1;
    }

    HttpsDownloader_Reset();
    if (Downloader_ParseUrl(url) != 0)
    {
        Downloader_SetError("invalid_https_url");
        downloader.state = DOWNLOADER_STATE_FAILED;
        downloader.status = HTTPS_DOWNLOADER_FAILED;
        return -1;
    }

    downloader.staging_request = *request;
    downloader.expected_size = request->file_size_bytes;
    memcpy(downloader.expected_sha256,
           request->expected_sha256,
           sizeof(downloader.expected_sha256));
    downloader.attempt = 1U;
    downloader.total_deadline_ms = HAL_GetTick() + HTTPS_TOTAL_TIMEOUT_MS;
    Downloader_ResetAttemptBuffers();
    downloader.state = DOWNLOADER_STATE_DNS_START;
    downloader.status = HTTPS_DOWNLOADER_RUNNING;
    Downloader_SetError("none");
    Downloader_Logf("HTTPS: download started size=%lu attempt=1/%u",
                    (unsigned long)request->file_size_bytes,
                    (unsigned int)HTTPS_MAX_ATTEMPTS);
    return 0;
}

int HttpsDownloader_Process(void)
{
    if (downloader.status == HTTPS_DOWNLOADER_SUCCEEDED)
    {
        return 1;
    }
    if (downloader.status == HTTPS_DOWNLOADER_FAILED)
    {
        return -1;
    }
    if (downloader.status != HTTPS_DOWNLOADER_RUNNING)
    {
        return 0;
    }

    if (Downloader_DeadlineReached(downloader.total_deadline_ms))
    {
        Downloader_Fail("download_timeout", 0);
        return -1;
    }

    switch (downloader.state)
    {
    case DOWNLOADER_STATE_RETRY_WAIT:
        if (Downloader_DeadlineReached(downloader.retry_after_ms))
        {
            Downloader_ResetAttemptBuffers();
            downloader.state = DOWNLOADER_STATE_DNS_START;
        }
        break;

    case DOWNLOADER_STATE_DNS_START:
        Downloader_StartDns();
        break;

    case DOWNLOADER_STATE_DNS_WAIT:
        if (downloader.dns_done != 0U)
        {
            if ((downloader.dns_result != ERR_OK) ||
                !IP_IS_V4(&downloader.dns_address))
            {
                Downloader_Fail("dns_failed", 1);
                break;
            }
            ipaddr_ntoa_r(&downloader.dns_address,
                          downloader.resolved_ip,
                          sizeof(downloader.resolved_ip));
            downloader.state = DOWNLOADER_STATE_TCP_START;
            Downloader_Log("HTTPS: DNS resolved");
        }
        else if (Downloader_DeadlineReached(downloader.state_deadline_ms))
        {
            Downloader_Fail("dns_timeout", 1);
        }
        break;

    case DOWNLOADER_STATE_TCP_START:
        (void)Downloader_TcpStart();
        break;

    case DOWNLOADER_STATE_TCP_WAIT:
        Downloader_TcpPoll();
        break;

    case DOWNLOADER_STATE_TLS_SETUP:
        Downloader_TlsSetup();
        break;

    case DOWNLOADER_STATE_TLS_HANDSHAKE:
        Downloader_TlsHandshake();
        break;

    case DOWNLOADER_STATE_SEND_REQUEST:
        Downloader_SendRequest();
        break;

    case DOWNLOADER_STATE_FLASH_PREPARE:
        return Downloader_PrepareFlash();

    case DOWNLOADER_STATE_READ_RESPONSE:
        return Downloader_ReadResponse();

    default:
        break;
    }

    return downloader.status == HTTPS_DOWNLOADER_FAILED ? -1 : 0;
}

void HttpsDownloader_Abort(void)
{
    if (!downloader_initialized)
    {
        return;
    }
    if (downloader.flash_started != 0U)
    {
        (void)FlashStaging_Abort();
        downloader.flash_started = 0U;
    }
    Downloader_InvalidateDns();
    Downloader_FreeTls();
    downloader.state = DOWNLOADER_STATE_IDLE;
    downloader.status = HTTPS_DOWNLOADER_IDLE;
}

void HttpsDownloader_Reset(void)
{
    HttpsDownloader_Abort();
    downloader.expected_size = 0U;
    downloader.bytes_received = 0U;
    downloader.host[0] = '\0';
    downloader.target[0] = '\0';
    memset(downloader.expected_sha256, 0, sizeof(downloader.expected_sha256));
    memset(&downloader.staging_request, 0,
           sizeof(downloader.staging_request));
    Downloader_SetError("none");
}

HttpsDownloaderStatus HttpsDownloader_GetStatus(void)
{
    return downloader.status;
}

uint32_t HttpsDownloader_GetBytesReceived(void)
{
    return downloader.bytes_received;
}

uint32_t HttpsDownloader_GetExpectedSize(void)
{
    return downloader.expected_size;
}

const char *HttpsDownloader_GetLastError(void)
{
    return downloader.last_error;
}
