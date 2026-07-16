#include "tls_client.h"
#include "aws_iot_config.h"
#include "mqtt_topics.h"
#include "aws_certs.h"
#include "debug_logger.h"
#include "device_config.h"
#include "main.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/version.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef ECONNRESET
#define ECONNRESET 104
#endif

#ifndef AWS_IOT_STATIC_IP
#define AWS_IOT_STATIC_IP ""
#endif

#ifndef TLS_VERBOSE_IO_LOGS
#define TLS_VERBOSE_IO_LOGS 0
#endif

#ifndef TLS_VERBOSE_DIAG_LOGS
#define TLS_VERBOSE_DIAG_LOGS 0
#endif

#define TLS_TCP_CONNECT_TIMEOUT_MS     10000U
#define TLS_HANDSHAKE_TIMEOUT_MS       60000U
#define TLS_WAIT_LOG_INTERVAL_MS       5000U
#define TLS_SOCKET_IO_TIMEOUT_US       10000L
#define TLS_SOCKET_IO_MAX_CHUNK        1024U

#define MQTT_RX_BUFFER_SIZE            1024U
#define MQTT_JSON_BUFFER_SIZE          768U

#ifndef STM32_APP_FIRMWARE_VERSION
#define STM32_APP_FIRMWARE_VERSION     "0.1.0"
#endif

extern volatile uint32_t eth_rx_irq_count;
extern volatile uint32_t eth_input_pbuf_count;
extern volatile uint32_t eth_err_irq_count;
extern volatile uint32_t eth_last_dma_error;
extern volatile uint32_t eth_last_hal_error;
extern volatile uint32_t eth_rx_alloc_count;
extern volatile uint32_t eth_rx_free_count;
extern volatile uint32_t eth_rx_alloc_fail_count;

static mbedtls_net_context tls_net;
static mbedtls_ssl_context tls_ssl;
static mbedtls_ssl_config tls_conf;
static mbedtls_x509_crt tls_ca_cert;
static mbedtls_x509_crt tls_client_cert;
static mbedtls_pk_context tls_pkey;
static mbedtls_ctr_drbg_context tls_ctr_drbg;
static mbedtls_entropy_context tls_entropy;

static int tls_connected = 0;
static int tls_ctx_ready = 0;
static volatile uint32_t tls_send_calls = 0;
static volatile uint32_t tls_recv_calls = 0;
static volatile int tls_last_send_ret = 0;
static volatile int tls_last_send_errno = 0;
static volatile int tls_last_recv_ret = 0;
static volatile int tls_last_recv_errno = 0;
static volatile int tls_dns_done = 0;
static volatile err_t tls_dns_err = ERR_INPROGRESS;
static volatile int tls_dns_start_done = 0;
static volatile err_t tls_dns_start_err = ERR_INPROGRESS;
static ip_addr_t tls_dns_addr;
static char tls_dns_hostname[256];
static int mqtt_ready = 0;
static uint32_t mqtt_last_telemetry_ms = 0U;
static uint32_t mqtt_last_ping_ms = 0U;
static uint16_t mqtt_negotiated_keep_alive_seconds = 60U;
static uint8_t mqtt_rx_buffer[MQTT_RX_BUFFER_SIZE];
static size_t mqtt_rx_length = 0U;

typedef struct
{
    const char *name;
    uint8_t mask;
} MqttMetricDefinition;

static const MqttMetricDefinition mqtt_metric_definitions[] = {
    { "temperature", DEVICE_METRIC_TEMPERATURE },
    { "voltage", DEVICE_METRIC_VOLTAGE },
    { "current", DEVICE_METRIC_CURRENT },
    { "signal", DEVICE_METRIC_SIGNAL },
    { "humidity", DEVICE_METRIC_HUMIDITY }
};

static void Tls_DnsFound(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)arg;

    if (ipaddr != NULL)
    {
        tls_dns_addr = *ipaddr;
        tls_dns_err = ERR_OK;
    }
    else
    {
        tls_dns_err = ERR_VAL;
    }

    tls_dns_done = 1;
}

static void Tls_DnsStartOnTcpipThread(void *arg)
{
    ip_addr_t addr;
    err_t err;

    (void)arg;

    err = dns_gethostbyname(tls_dns_hostname, &addr, Tls_DnsFound, NULL);
    tls_dns_start_err = err;

    if (err == ERR_OK)
    {
        tls_dns_addr = addr;
        tls_dns_err = ERR_OK;
        tls_dns_done = 1;
    }
    else if (err != ERR_INPROGRESS)
    {
        tls_dns_err = err;
        tls_dns_done = 1;
    }

    tls_dns_start_done = 1;
}

static const int aws_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    0
};

static void tls_log(const char *msg)
{
    Debug_LogLine(DEBUG_SRC_TLS, msg);
}

static void tls_logf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    buf[sizeof(buf) - 1U] = '\0';
    tls_log(buf);
}

#if TLS_VERBOSE_IO_LOGS
#define tls_log_io(text)     tls_log(text)
#define tls_log_iof(...)     tls_logf(__VA_ARGS__)
#else
#define tls_log_io(text)     ((void)0)
#define tls_log_iof(...)     ((void)0)
#endif

#if TLS_VERBOSE_DIAG_LOGS
#define tls_log_diag(text)   tls_log(text)
#define tls_log_diagf(...)   tls_logf(__VA_ARGS__)
#else
#define tls_log_diag(text)   ((void)0)
#define tls_log_diagf(...)   ((void)0)
#endif

static const char *tls_state_name(int state)
{
    switch (state)
    {
    case MBEDTLS_SSL_HELLO_REQUEST:
        return "HELLO_REQUEST";
    case MBEDTLS_SSL_CLIENT_HELLO:
        return "CLIENT_HELLO";
    case MBEDTLS_SSL_SERVER_HELLO:
        return "SERVER_HELLO";
    case MBEDTLS_SSL_SERVER_CERTIFICATE:
        return "SERVER_CERTIFICATE";
    case MBEDTLS_SSL_SERVER_KEY_EXCHANGE:
        return "SERVER_KEY_EXCHANGE";
    case MBEDTLS_SSL_CERTIFICATE_REQUEST:
        return "CERTIFICATE_REQUEST";
    case MBEDTLS_SSL_SERVER_HELLO_DONE:
        return "SERVER_HELLO_DONE";
    case MBEDTLS_SSL_CLIENT_CERTIFICATE:
        return "CLIENT_CERTIFICATE";
    case MBEDTLS_SSL_CLIENT_KEY_EXCHANGE:
        return "CLIENT_KEY_EXCHANGE";
    case MBEDTLS_SSL_CERTIFICATE_VERIFY:
        return "CERTIFICATE_VERIFY";
    case MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC:
        return "CLIENT_CHANGE_CIPHER_SPEC";
    case MBEDTLS_SSL_CLIENT_FINISHED:
        return "CLIENT_FINISHED";
    case MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC:
        return "SERVER_CHANGE_CIPHER_SPEC";
    case MBEDTLS_SSL_SERVER_FINISHED:
        return "SERVER_FINISHED";
    case MBEDTLS_SSL_FLUSH_BUFFERS:
        return "FLUSH_BUFFERS";
    case MBEDTLS_SSL_HANDSHAKE_WRAPUP:
        return "HANDSHAKE_WRAPUP";
    case MBEDTLS_SSL_HANDSHAKE_OVER:
        return "HANDSHAKE_OVER";
    case MBEDTLS_SSL_SERVER_NEW_SESSION_TICKET:
        return "SERVER_NEW_SESSION_TICKET";
    case MBEDTLS_SSL_SERVER_HELLO_VERIFY_REQUEST_SENT:
        return "SERVER_HELLO_VERIFY_REQUEST_SENT";
    default:
        return "UNKNOWN";
    }
}

static void tls_log_mbedtls_error(const char *what, int ret)
{
    char errbuf[160];
    char logbuf[256];

    mbedtls_strerror(ret, errbuf, sizeof(errbuf));
    snprintf(logbuf, sizeof(logbuf),
             "%s failed: ret=-0x%04X (%s)",
             what,
             (unsigned int)(-ret),
             errbuf);
    tls_log(logbuf);
}

static void tls_log_verify_result_now(void)
{
    uint32_t flags = mbedtls_ssl_get_verify_result(&tls_ssl);

    tls_logf("TLS: verify_flags=0x%08lx", (unsigned long)flags);

    if (flags != 0U)
    {
        char vrfy_buf[512];
        memset(vrfy_buf, 0, sizeof(vrfy_buf));

        mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", flags);

        tls_log("TLS: verify info:");
        tls_log(vrfy_buf);
    }
    else
    {
        tls_log("TLS: verify_flags empty/no X509 detail available");
    }
}

static void tls_ctx_init_all(void)
{
    tls_log_diag("CTX: manual net init");
    tls_net.fd = -1;
    tls_log_diag("CTX: manual net init OK");

    tls_log_diag("CTX: ssl init");
    mbedtls_ssl_init(&tls_ssl);

    tls_log_diag("CTX: ssl config init");
    mbedtls_ssl_config_init(&tls_conf);

    tls_log_diag("CTX: ca cert init");
    mbedtls_x509_crt_init(&tls_ca_cert);

    tls_log_diag("CTX: client cert init");
    mbedtls_x509_crt_init(&tls_client_cert);

    tls_log_diag("CTX: pk init");
    mbedtls_pk_init(&tls_pkey);

    tls_log_diag("CTX: ctr drbg init");
    mbedtls_ctr_drbg_init(&tls_ctr_drbg);

    tls_log_diag("CTX: entropy init");
    mbedtls_entropy_init(&tls_entropy);

    tls_log_diag("CTX: all init OK");

    tls_ctx_ready = 1;
}

static void tls_ctx_free_all(void)
{
    if (!tls_ctx_ready)
    {
        return;
    }

    if (tls_connected)
    {
        (void)mbedtls_ssl_close_notify(&tls_ssl);
    }

    mbedtls_net_free(&tls_net);
    mbedtls_ssl_free(&tls_ssl);
    mbedtls_ssl_config_free(&tls_conf);
    mbedtls_x509_crt_free(&tls_ca_cert);
    mbedtls_x509_crt_free(&tls_client_cert);
    mbedtls_pk_free(&tls_pkey);
    mbedtls_ctr_drbg_free(&tls_ctr_drbg);
    mbedtls_entropy_free(&tls_entropy);

    tls_connected = 0;
    tls_ctx_ready = 0;
}

int TlsClient_Init(void)
{
    const DeviceConfig *config;

    tls_log("TLS: init");
    tls_log("TLS BUILD: mqtt_config_v1 dual_slot_flash");
    tls_connected = 0;
    tls_ctx_ready = 0;
    DeviceConfig_Init();
    config = DeviceConfig_Get();
    tls_logf("CONFIG: loaded version=%lu telemetry=%lus keepalive=%lus",
             (unsigned long)config->config_version,
             (unsigned long)config->telemetry_frequency_seconds,
             (unsigned long)config->mqtt_keep_alive_seconds);
    return 0;
}

static int Tls_ResolveHost(const char *host,
                           char *ipbuf,
                           size_t ipbuf_len,
                           uint32_t timeout_ms)
{
    err_t err;
    uint32_t start;

    tls_logf("TLS DNS: resolve %s", host);
    tls_log_diagf("TLS DNS: server0=%s", ipaddr_ntoa(dns_getserver(0)));

    tls_dns_done = 0;
    tls_dns_err = ERR_INPROGRESS;
    tls_dns_start_done = 0;
    tls_dns_start_err = ERR_INPROGRESS;
    memset(&tls_dns_addr, 0, sizeof(tls_dns_addr));
    snprintf(tls_dns_hostname, sizeof(tls_dns_hostname), "%s", host);
    tls_dns_hostname[sizeof(tls_dns_hostname) - 1U] = '\0';

    tls_log_diag("TLS DNS: before tcpip_callback dns_gethostbyname");

    err = tcpip_callback(Tls_DnsStartOnTcpipThread, NULL);

    tls_log_diagf("TLS DNS: tcpip_callback err=%d", (int)err);

    if (err != ERR_OK)
    {
        return -1;
    }

    start = HAL_GetTick();

    while (!tls_dns_start_done)
    {
        if ((HAL_GetTick() - start) > 2000U)
        {
            tls_log("TLS DNS: tcpip_callback start TIMEOUT");
            return -1;
        }

        osDelay(10);
    }

    tls_log_diagf("TLS DNS: start err=%d", (int)tls_dns_start_err);

    while (!tls_dns_done)
    {
        if ((HAL_GetTick() - start) > timeout_ms)
        {
            tls_log("TLS DNS: TIMEOUT");
            return -1;
        }

        osDelay(100);
    }

    tls_log_diagf("TLS DNS: callback err=%d", (int)tls_dns_err);

    if (tls_dns_err != ERR_OK)
    {
        tls_log("TLS DNS: resolve failed async");
        return -1;
    }

    ipaddr_ntoa_r(&tls_dns_addr, ipbuf, ipbuf_len);
    tls_logf("TLS DNS: resolved async -> %s", ipbuf);

    return 0;
}

static int Tls_LwipConnectIp(mbedtls_net_context *ctx,
                             const char *ip,
                             uint16_t port,
                             uint32_t timeout_ms)
{
    int sock;
    struct sockaddr_in addr;
    int ret;

    tls_logf("TLS: lwip socket connect %s:%u", ip, port);

    tls_log_diag("TLS CONN: before lwip_socket()");
    sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    tls_log_diagf("TLS CONN: after lwip_socket() sock=%d errno=%d", sock, errno);

    if (sock < 0)
    {
        tls_logf("TLS: socket failed=%d errno=%d", sock, errno);
        return -1;
    }

    int nonblock = 1;

    tls_log_diag("TLS CONN: before ioctl nonblock");
    ret = lwip_ioctl(sock, FIONBIO, &nonblock);
    tls_log_diagf("TLS CONN: after ioctl ret=%d errno=%d", ret, errno);

    if (ret < 0)
    {
        tls_logf("TLS: ioctl nonblock failed errno=%d", errno);
        lwip_close(sock);
        return -2;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    errno = 0;
    tls_log_diag("TLS CONN: before lwip_connect()");
    ret = lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    tls_log_diagf("TLS CONN: after lwip_connect ret=%d errno=%d", ret, errno);

    if (ret != 0)
    {
        uint32_t start = HAL_GetTick();
        int connect_errno = errno;

        if ((connect_errno != EINPROGRESS) &&
            (connect_errno != EALREADY) &&
            (connect_errno != EWOULDBLOCK) &&
            (connect_errno != EAGAIN))
        {
            tls_logf("TLS: connect immediate error errno=%d", connect_errno);
            lwip_close(sock);
            return -4;
        }

        for (;;)
        {
            fd_set write_fds;
            int err = 0;
            socklen_t len = sizeof(err);
            struct timeval tv;
            uint32_t elapsed = HAL_GetTick() - start;
            int sel_ret;

            if (elapsed > timeout_ms)
            {
                tls_logf("TLS: connect timeout after %lu ms", (unsigned long)elapsed);
                lwip_close(sock);
                return -3;
            }

            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);
            tv.tv_sec = 0;
            tv.tv_usec = 100000;

            errno = 0;
            sel_ret = lwip_select(sock + 1, NULL, &write_fds, NULL, &tv);

            if (sel_ret == 0)
            {
                osDelay(10);
                continue;
            }

            if (sel_ret < 0)
            {
                tls_logf("TLS: connect select failed ret=%d errno=%d", sel_ret, errno);
                lwip_close(sock);
                return -4;
            }

            if (!FD_ISSET(sock, &write_fds))
            {
                osDelay(10);
                continue;
            }

            errno = 0;
            ret = lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            tls_log_iof("TLS CONN: writable ret=%d err=%d errno=%d t=%lu",
                        ret,
                        err,
                        errno,
                        (unsigned long)elapsed);

            if ((ret == 0) && (err == 0))
            {
                break;
            }

            if ((ret != 0) ||
                ((err != 0) &&
                 (err != EINPROGRESS) &&
                 (err != EALREADY) &&
                 (err != EWOULDBLOCK) &&
                 (err != EAGAIN)))
            {
                tls_logf("TLS: connect socket error ret=%d err=%d errno=%d", ret, err, errno);
                lwip_close(sock);
                return -4;
            }

            osDelay(50);
        }
    }

    ctx->fd = sock;

    struct timeval rcv_to;
    /*
     * Socket jest ustawiony jako nieblokujacy, ale krotki timeout jest
     * dodatkowym zabezpieczeniem, gdyby LwIP zignorowal MSG_DONTWAIT.
     */
    rcv_to.tv_sec = 0;
    rcv_to.tv_usec = TLS_SOCKET_IO_TIMEOUT_US;

    struct timeval snd_to;
    snd_to.tv_sec = 0;
    snd_to.tv_usec = TLS_SOCKET_IO_TIMEOUT_US;

    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

    tls_logf("TLS: lwip socket connected fd=%d nonblock+timeout OK", sock);
    return 0;
}

static int Tls_LwipSend(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;
    size_t to_send = len;
    int ret;

    tls_send_calls++;

    if (to_send > 1024)
    {
        to_send = 1024;
    }

    errno = 0;
    ret = lwip_send(fd, buf, to_send, MSG_DONTWAIT);
    tls_last_send_ret = ret;
    tls_last_send_errno = errno;

    if (ret > 0)
    {
        return ret;
    }

    if (ret == 0)
    {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS)
    {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    tls_logf("TLS SEND: failed ret=%d errno=%d", ret, errno);
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int Tls_LwipRecv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;
    int ret;
    size_t to_recv = len;

    tls_recv_calls++;

    /*
     * MSG_DONTWAIT jest obslugiwane bezposrednio przez lwIP jako
     * NETCONN_DONTBLOCK (sys_arch_mbox_tryfetch). Nie uzywamy tu select(),
     * poniewaz jego obsluga deskryptorow potrafila zatrzymac kolejne kroki
     * handshake po pierwszym MBEDTLS_ERR_SSL_WANT_READ.
     */
    if (to_recv > TLS_SOCKET_IO_MAX_CHUNK)
    {
        to_recv = TLS_SOCKET_IO_MAX_CHUNK;
    }

    errno = 0;
    ret = lwip_recv(fd, buf, to_recv, MSG_DONTWAIT);
    tls_last_recv_ret = ret;
    tls_last_recv_errno = errno;

    if (ret > 0)
    {
        return ret;
    }

    if (ret == 0)
    {
        tls_logf("TLS RECV: connection closed ret=0 errno=%d", errno);
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS)
    {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    if (errno == ECONNRESET)
    {
        tls_logf("TLS RECV: reset ret=%d errno=%d", ret, errno);
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    tls_logf("TLS RECV: failed ret=%d errno=%d", ret, errno);
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

int TlsClient_Connect(void)
{
    int ret;
    char port_str[8];
    char aws_ip[48] = {0};
    const char *pers = "stm32-aws-iot";
    uint32_t verify_flags = 0;

    tls_log("TLS: connect start");
    tls_logf("TLS: mbedTLS version=%s", MBEDTLS_VERSION_STRING);
    tls_send_calls = 0;
    tls_recv_calls = 0;
    tls_last_send_ret = 0;
    tls_last_send_errno = 0;
    tls_last_recv_ret = 0;
    tls_last_recv_errno = 0;

    tls_log_diag("TLS: free ctx...");
    tls_ctx_free_all();
    tls_log_diag("TLS: free ctx OK");

    tls_log_diag("TLS: init ctx...");
    tls_ctx_init_all();
    tls_log("TLS: init ctx OK");

    tls_log("TLS: seeding RNG...");
    ret = mbedtls_ctr_drbg_seed(&tls_ctr_drbg,
                                mbedtls_entropy_func,
                                &tls_entropy,
                                (const unsigned char *)pers,
                                strlen(pers));
    tls_log_diag("TLS: seed returned");

    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_ctr_drbg_seed", ret);
        goto fail;
    }

    tls_log("TLS: RNG seed OK");

    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)AWS_IOT_PORT);

    if (AWS_IOT_STATIC_IP[0] != '\0')
    {
        (void)snprintf(aws_ip, sizeof(aws_ip), "%s", AWS_IOT_STATIC_IP);
        tls_logf("TLS: using static AWS IP=%s", aws_ip);
    }
    else
    {
        tls_logf("TLS: resolve AWS HOST %s", AWS_IOT_ENDPOINT);

        ret = Tls_ResolveHost(AWS_IOT_ENDPOINT, aws_ip, sizeof(aws_ip), 15000);
        if (ret != 0)
        {
            tls_logf("TLS: host resolve failed ret=%d", ret);
            goto fail;
        }

        tls_logf("TLS: AWS resolved IP=%s", aws_ip);
    }

    ret = mbedtls_x509_crt_parse(&tls_ca_cert,
                                 (const unsigned char *)AWS_ROOT_CA_PEM,
                                 sizeof(AWS_ROOT_CA_PEM));
    if (ret < 0)
    {
        tls_log_mbedtls_error("mbedtls_x509_crt_parse(root_ca)", ret);
        goto fail;
    }

    tls_log_diag("TLS: Root CA parsed");

    ret = mbedtls_x509_crt_parse(&tls_client_cert,
                                 (const unsigned char *)AWS_DEVICE_CERT_PEM,
                                 sizeof(AWS_DEVICE_CERT_PEM));
    if (ret < 0)
    {
        tls_log_mbedtls_error("mbedtls_x509_crt_parse(device_cert)", ret);
        goto fail;
    }

    tls_log_diag("TLS: device cert parsed");

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    ret = mbedtls_pk_parse_key(&tls_pkey,
                               (const unsigned char *)AWS_PRIVATE_KEY_PEM,
                               sizeof(AWS_PRIVATE_KEY_PEM),
                               NULL,
                               0,
                               mbedtls_ctr_drbg_random,
                               &tls_ctr_drbg);
#else
    ret = mbedtls_pk_parse_key(&tls_pkey,
                               (const unsigned char *)AWS_PRIVATE_KEY_PEM,
                               sizeof(AWS_PRIVATE_KEY_PEM),
                               NULL,
                               0);
#endif

    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_pk_parse_key(private_key)", ret);
        goto fail;
    }

    tls_log_diag("TLS: private key parsed");

    ret = mbedtls_pk_check_pair(&tls_client_cert.pk, &tls_pkey);
    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_pk_check_pair(cert,key)", ret);
        goto fail;
    }

    tls_log("TLS: cert/key pair OK");
    tls_logf("TLS: TCP connect AWS IP %s:%s", aws_ip, port_str);

    ret = -1;

    for (int attempt = 1; attempt <= 3; attempt++)
    {
        tls_logf("TLS: TCP connect attempt %d/3", attempt);

        ret = Tls_LwipConnectIp(&tls_net,
                                aws_ip,
                                AWS_IOT_PORT,
                                TLS_TCP_CONNECT_TIMEOUT_MS);

        if (ret == 0)
        {
            break;
        }

        tls_logf("TLS: TCP connect attempt failed ret=%d", ret);

        osDelay(1000);
    }

    if (ret != 0)
    {
        tls_logf("TLS: TCP connect failed ret=%d", ret);
        goto fail;
    }

    tls_log("TLS: TCP connected");

    ret = mbedtls_ssl_config_defaults(&tls_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_ssl_config_defaults", ret);
        goto fail;
    }

    mbedtls_ssl_conf_min_version(&tls_conf,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);

    mbedtls_ssl_conf_max_version(&tls_conf,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);

    /*
     * Wymuszamy sprawdzony zestaw TLS 1.2 dla AWS IoT i certyfikatu RSA.
     * Dzieki temu handshake nie wybiera przypadkowo innego cipher suite.
     */
    mbedtls_ssl_conf_ciphersuites(&tls_conf, aws_ciphersuites);

    mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls_conf, &tls_ca_cert, NULL);
    mbedtls_ssl_conf_rng(&tls_conf, mbedtls_ctr_drbg_random, &tls_ctr_drbg);

    ret = mbedtls_ssl_conf_own_cert(&tls_conf, &tls_client_cert, &tls_pkey);
    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_ssl_conf_own_cert", ret);
        goto fail;
    }

    ret = mbedtls_ssl_setup(&tls_ssl, &tls_conf);
    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_ssl_setup", ret);
        goto fail;
    }

    ret = mbedtls_ssl_set_hostname(&tls_ssl, AWS_IOT_ENDPOINT);
    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_ssl_set_hostname", ret);
        goto fail;
    }

    tls_log_diagf("TLS: hostname/SNI set to %s", AWS_IOT_ENDPOINT);

    tls_log_diag("TLS: set bio custom lwip");
    mbedtls_ssl_set_bio(&tls_ssl, &tls_net, Tls_LwipSend, Tls_LwipRecv, NULL);

    tls_log("TLS: handshake...");

    uint32_t hs_start = HAL_GetTick();
    uint32_t hs_last_log = HAL_GetTick();
    uint32_t hs_last_mem_log = HAL_GetTick();
    int hs_try = 0;
    int hs_last_state = -1;
    int hs_last_after_state = -1;
    int hs_last_public_state = -1;
    int hs_server_hello_before_logged = 0;
    int hs_server_hello_after_logged = 0;

    ret = 0;

    while (tls_ssl.state != MBEDTLS_SSL_HANDSHAKE_OVER)
    {
        uint32_t step_start;
        uint32_t now;
        int state_before;

        hs_try++;
        state_before = tls_ssl.state;
        step_start = HAL_GetTick();

        if ((state_before == MBEDTLS_SSL_SERVER_HELLO) &&
            (hs_server_hello_before_logged == 0))
        {
            hs_server_hello_before_logged = 1;
            tls_logf("TLS: before SERVER_HELLO tr=%d rc=%lu rr=%d re=%d rx=%lu pb=%lu al=%lu fr=%lu af=%lu",
                     hs_try,
                     (unsigned long)tls_recv_calls,
                     tls_last_recv_ret,
                     tls_last_recv_errno,
                     (unsigned long)eth_rx_irq_count,
                     (unsigned long)eth_input_pbuf_count,
                     (unsigned long)eth_rx_alloc_count,
                     (unsigned long)eth_rx_free_count,
                     (unsigned long)eth_rx_alloc_fail_count);
        }

        if ((hs_try <= 20) || (state_before != hs_last_state))
        {
            hs_last_state = state_before;
            tls_log_diagf("TLS STEP: before tr=%d state=%s(%d)",
                          hs_try,
                          tls_state_name(state_before),
                          state_before);
        }

        ret = mbedtls_ssl_handshake_step(&tls_ssl);
        now = HAL_GetTick();

        if ((state_before == MBEDTLS_SSL_SERVER_HELLO) &&
            (hs_server_hello_after_logged == 0))
        {
            hs_server_hello_after_logged = 1;
            tls_logf("TLS: after SERVER_HELLO ret=%d state=%s(%d) dt=%lu rr=%d re=%d rx=%lu pb=%lu al=%lu fr=%lu af=%lu",
                     ret,
                     tls_state_name(tls_ssl.state),
                     tls_ssl.state,
                     (unsigned long)(now - step_start),
                     tls_last_recv_ret,
                     tls_last_recv_errno,
                     (unsigned long)eth_rx_irq_count,
                     (unsigned long)eth_input_pbuf_count,
                     (unsigned long)eth_rx_alloc_count,
                     (unsigned long)eth_rx_free_count,
                     (unsigned long)eth_rx_alloc_fail_count);
        }
#if !TLS_VERBOSE_DIAG_LOGS
        (void)step_start;
#endif

        if ((hs_try <= 20) ||
            (tls_ssl.state != hs_last_after_state) ||
            ((ret != 0) &&
             (ret != MBEDTLS_ERR_SSL_WANT_READ) &&
             (ret != MBEDTLS_ERR_SSL_WANT_WRITE)))
        {
            hs_last_after_state = tls_ssl.state;
            tls_log_diagf("TLS STEP: after tr=%d state=%s(%d) ret=%d dt=%lu",
                          hs_try,
                          tls_state_name(tls_ssl.state),
                          tls_ssl.state,
                          ret,
                          (unsigned long)(now - step_start));
        }

        if (tls_ssl.state != hs_last_public_state)
        {
            hs_last_public_state = tls_ssl.state;
            tls_logf("TLS: state=%s(%d)", tls_state_name(tls_ssl.state), tls_ssl.state);
        }

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            if ((now - hs_last_log) >= TLS_WAIT_LOG_INTERVAL_MS)
            {
                hs_last_log = now;

                /* Do not call synchronous getsockopt() here. If the tcpip
                 * mailbox is busy, it can block this diagnostic path and
                 * prevent the handshake timeout from being evaluated. */
                tls_logf("TLS WAIT: t=%lu tr=%d sc=%lu rc=%lu sr=%d se=%d rr=%d re=%d rx=%lu pb=%lu er=%lu al=%lu fr=%lu af=%lu",
                         (unsigned long)(now - hs_start),
                         hs_try,
                         (unsigned long)tls_send_calls,
                         (unsigned long)tls_recv_calls,
                         tls_last_send_ret,
                         tls_last_send_errno,
                         tls_last_recv_ret,
                         tls_last_recv_errno,
                         eth_rx_irq_count,
                         eth_input_pbuf_count,
                         eth_err_irq_count,
                         eth_rx_alloc_count,
                         eth_rx_free_count,
                         eth_rx_alloc_fail_count);
            }

            if ((now - hs_last_mem_log) >= 10000U)
            {
                hs_last_mem_log = now;

                tls_log_diagf("TLS MEM: stack=%lu heap=%lu min=%lu",
                              (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                              (unsigned long)xPortGetFreeHeapSize(),
                              (unsigned long)xPortGetMinimumEverFreeHeapSize());
            }

            if ((now - hs_start) >= TLS_HANDSHAKE_TIMEOUT_MS)
            {
                tls_log("TLS: handshake TIMEOUT");
                goto fail;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else if (ret != 0)
        {
            break;
        }
        else
        {
            if ((now - hs_start) >= TLS_HANDSHAKE_TIMEOUT_MS)
            {
                tls_log("TLS: handshake TIMEOUT");
                goto fail;
            }
        }
    }

    if (ret != 0)
    {
        tls_logf("TLS: handshake raw ret=%d", ret);
        tls_log_mbedtls_error("mbedtls_ssl_handshake", ret);
        tls_log_verify_result_now();
        goto fail;
    }

    tls_log("TLS: handshake function OK");

    verify_flags = mbedtls_ssl_get_verify_result(&tls_ssl);

    tls_logf("TLS: verify_flags=0x%08lx", verify_flags);

    if (verify_flags != 0U)
    {
        char vrfy_buf[512];

        mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", verify_flags);
        tls_log("TLS: verify info:");
        tls_log(vrfy_buf);

        goto fail;
    }
    else
    {
        tls_log("TLS: certificate verify OK");
    }

    tls_connected = 1;
    tls_log("TLS: HANDSHAKE OK");
    return 0;

fail:
    tls_log("TLS: connect failed");
    tls_ctx_free_all();
    return -1;
}

int TlsClient_Write(const uint8_t *data, size_t len)
{
    int ret;

    if (!tls_connected)
    {
        tls_log("TLS: write refused, not connected");
        return -1;
    }

    ret = mbedtls_ssl_write(&tls_ssl, data, len);
    if (ret < 0)
    {
        tls_log_mbedtls_error("mbedtls_ssl_write", ret);
        return -1;
    }

    return ret;
}

int TlsClient_Read(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    int ret;
    (void)timeout_ms;

    if (!tls_connected)
    {
        tls_log("TLS: read refused, not connected");
        return -1;
    }

    ret = mbedtls_ssl_read(&tls_ssl, data, len);
    if (ret < 0 &&
        ret != MBEDTLS_ERR_SSL_WANT_READ &&
        ret != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        tls_log_mbedtls_error("mbedtls_ssl_read", ret);
        return -1;
    }

    return ret;
}

static size_t mqtt_encode_remaining_length(uint8_t *out, size_t len)
{
    size_t written = 0U;

    do
    {
        uint8_t encoded = (uint8_t)(len % 128U);
        len /= 128U;

        if (len > 0U)
        {
            encoded |= 0x80U;
        }

        out[written++] = encoded;
    }
    while (len > 0U);

    return written;
}

static int mqtt_put_string(uint8_t *buf,
                           size_t buf_len,
                           size_t *pos,
                           const char *text)
{
    size_t len = strlen(text);

    if ((len > 65535U) || ((*pos + 2U + len) > buf_len))
    {
        return -1;
    }

    buf[(*pos)++] = (uint8_t)((len >> 8) & 0xFFU);
    buf[(*pos)++] = (uint8_t)(len & 0xFFU);
    memcpy(&buf[*pos], text, len);
    *pos += len;

    return 0;
}

static int tls_write_all(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    size_t sent = 0U;
    uint32_t start = HAL_GetTick();

    while (sent < len)
    {
        int ret;

        ret = mbedtls_ssl_write(&tls_ssl, &data[sent], len - sent);

        if (ret > 0)
        {
            sent += (size_t)ret;
            continue;
        }

        if ((ret == MBEDTLS_ERR_SSL_WANT_READ) ||
            (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
        {
            if ((HAL_GetTick() - start) >= timeout_ms)
            {
                tls_log("MQTT/TLS: write timeout");
                return -1;
            }

            osDelay(10);
            continue;
        }

        tls_log_mbedtls_error("mbedtls_ssl_write", ret);
        return -1;
    }

    return 0;
}

static int tls_read_exact(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    size_t got = 0U;
    uint32_t start = HAL_GetTick();

    while (got < len)
    {
        int ret;

        ret = mbedtls_ssl_read(&tls_ssl, &data[got], len - got);

        if (ret > 0)
        {
            got += (size_t)ret;
            continue;
        }

        if ((ret == MBEDTLS_ERR_SSL_WANT_READ) ||
            (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
        {
            if ((HAL_GetTick() - start) >= timeout_ms)
            {
                tls_log("MQTT/TLS: read timeout");
                return -1;
            }

            osDelay(10);
            continue;
        }

        tls_log_mbedtls_error("mbedtls_ssl_read", ret);
        return -1;
    }

    return 0;
}

static int mqtt_send_connect(void)
{
    uint8_t pkt[256];
    uint8_t connack[4];
    const DeviceConfig *config = DeviceConfig_Get();
    size_t client_id_len = strlen(AWS_CLIENT_ID);
    size_t remaining = 10U + 2U + client_id_len;
    size_t rem_len;
    size_t pos;

    if (remaining > 127U)
    {
        tls_log("MQTT: CONNECT too large");
        return -1;
    }

    pkt[0] = 0x10U;
    rem_len = mqtt_encode_remaining_length(&pkt[1], remaining);
    pos = 1U + rem_len;

    if (mqtt_put_string(pkt, sizeof(pkt), &pos, "MQTT") != 0)
    {
        return -1;
    }

    pkt[pos++] = 4U;      /* MQTT 3.1.1 */
    pkt[pos++] = 0x02U;   /* clean session */
    pkt[pos++] = (uint8_t)((config->mqtt_keep_alive_seconds >> 8U) & 0xFFU);
    pkt[pos++] = (uint8_t)(config->mqtt_keep_alive_seconds & 0xFFU);

    if (mqtt_put_string(pkt, sizeof(pkt), &pos, AWS_CLIENT_ID) != 0)
    {
        return -1;
    }

    tls_logf("MQTT: CONNECT client=%s", AWS_CLIENT_ID);

    if (tls_write_all(pkt, pos, 5000U) != 0)
    {
        return -1;
    }

    if (tls_read_exact(connack, sizeof(connack), 5000U) != 0)
    {
        tls_log("MQTT: CONNACK read failed");
        return -1;
    }

    tls_logf("MQTT: CONNACK %02X %02X %02X %02X",
             connack[0],
             connack[1],
             connack[2],
             connack[3]);

    if ((connack[0] != 0x20U) || (connack[1] != 0x02U) || (connack[3] != 0x00U))
    {
        tls_log("MQTT: CONNECT refused");
        return -1;
    }

    tls_log("MQTT: CONNECT OK");
    mqtt_negotiated_keep_alive_seconds = config->mqtt_keep_alive_seconds;
    return 0;
}

static int mqtt_subscribe_topic(const char *topic, uint16_t packet_id)
{
    uint8_t pkt[256];
    uint8_t suback[5];
    size_t topic_len = strlen(topic);
    size_t remaining = 2U + 2U + topic_len + 1U;
    size_t rem_len;
    size_t pos;

    if (remaining > 127U)
    {
        tls_log("MQTT: SUBSCRIBE too large");
        return -1;
    }

    pkt[0] = 0x82U;
    rem_len = mqtt_encode_remaining_length(&pkt[1], remaining);
    pos = 1U + rem_len;
    pkt[pos++] = (uint8_t)((packet_id >> 8U) & 0xFFU);
    pkt[pos++] = (uint8_t)(packet_id & 0xFFU);

    if (mqtt_put_string(pkt, sizeof(pkt), &pos, topic) != 0)
    {
        return -1;
    }

    pkt[pos++] = 1U; /* QoS 1 */

    tls_logf("MQTT: SUBSCRIBE %s", topic);

    if (tls_write_all(pkt, pos, 5000U) != 0)
    {
        return -1;
    }

    if (tls_read_exact(suback, sizeof(suback), 5000U) != 0)
    {
        tls_log("MQTT: SUBACK read failed");
        return -1;
    }

    tls_logf("MQTT: SUBACK %02X %02X %02X %02X %02X",
             suback[0],
             suback[1],
             suback[2],
             suback[3],
             suback[4]);

    if ((suback[0] != 0x90U) ||
        (suback[1] != 0x03U) ||
        (suback[2] != (uint8_t)((packet_id >> 8U) & 0xFFU)) ||
        (suback[3] != (uint8_t)(packet_id & 0xFFU)) ||
        (suback[4] == 0x80U))
    {
        tls_log("MQTT: SUBSCRIBE refused");
        return -1;
    }

    tls_log("MQTT: SUBSCRIBE OK");
    return 0;
}

static int mqtt_publish_json(const char *topic, const char *payload)
{
    uint8_t pkt[1024];
    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t remaining;
    size_t rem_len;
    size_t pos;

    remaining = 2U + topic_len + payload_len;

    if (remaining > 268435455U)
    {
        tls_log("MQTT: PUBLISH too large");
        return -1;
    }

    pkt[0] = 0x30U; /* PUBLISH QoS 0 */
    rem_len = mqtt_encode_remaining_length(&pkt[1], remaining);
    pos = 1U + rem_len;

    if (mqtt_put_string(pkt, sizeof(pkt), &pos, topic) != 0)
    {
        return -1;
    }

    if ((pos + payload_len) > sizeof(pkt))
    {
        tls_log("MQTT: PUBLISH buffer too small");
        return -1;
    }

    memcpy(&pkt[pos], payload, payload_len);
    pos += payload_len;

    tls_logf("MQTT: PUBLISH %s", topic);

    if (tls_write_all(pkt, pos, 5000U) != 0)
    {
        return -1;
    }

    tls_log("MQTT: PUBLISH OK");
    return 0;
}

static int mqtt_publish_status(void)
{
    char payload[256];
    int written;

    written = snprintf(payload,
                       sizeof(payload),
                       "{\"deviceId\":\"%s\",\"schemaVersion\":1,"
                       "\"state\":\"online\",\"uptimeMs\":%lu,"
                       "\"firmwareVersion\":\"%s\"}",
                       MQTT_DEVICE_ID,
                       (unsigned long)HAL_GetTick(),
                       STM32_APP_FIRMWARE_VERSION);

    if ((written < 0) || ((size_t)written >= sizeof(payload)))
    {
        tls_log("MQTT: status JSON too large");
        return -1;
    }

    return mqtt_publish_json(MQTT_TOPIC_STATUS, payload);
}

static int mqtt_json_append(char *buffer, size_t buffer_size, size_t *length,
                            const char *format, ...)
{
    va_list args;
    int written;

    if (*length >= buffer_size)
    {
        return -1;
    }

    va_start(args, format);
    written = vsnprintf(&buffer[*length], buffer_size - *length, format, args);
    va_end(args);

    if ((written < 0) || ((size_t)written >= (buffer_size - *length)))
    {
        return -1;
    }

    *length += (size_t)written;
    return 0;
}

static int mqtt_metrics_json(uint8_t enabled_metrics, char *output, size_t output_size)
{
    size_t length = 0U;
    size_t index;
    int first = 1;

    if (mqtt_json_append(output, output_size, &length, "[") != 0)
    {
        return -1;
    }

    for (index = 0U;
         index < (sizeof(mqtt_metric_definitions) / sizeof(mqtt_metric_definitions[0]));
         index++)
    {
        if ((enabled_metrics & mqtt_metric_definitions[index].mask) == 0U)
        {
            continue;
        }

        if (mqtt_json_append(output,
                             output_size,
                             &length,
                             "%s\"%s\"",
                             first ? "" : ",",
                             mqtt_metric_definitions[index].name) != 0)
        {
            return -1;
        }
        first = 0;
    }

    return mqtt_json_append(output, output_size, &length, "]");
}

static int mqtt_publish_telemetry(void)
{
    char payload[MQTT_JSON_BUFFER_SIZE];
    const DeviceConfig *config = DeviceConfig_Get();
    uint32_t uptime_ms = HAL_GetTick();
    unsigned long simulated_temperature = 25UL + ((uptime_ms / 30000U) % 5U);
    unsigned long simulated_humidity = 45UL + ((uptime_ms / 60000U) % 10U);
    size_t length = 0U;

    /*
     * MVP: wartosci sa diagnostyczne/symulowane. Pozniej temperature i
     * napiecie nalezy podmienic na odczyty z rzeczywistych peryferiow.
     */
    if (mqtt_json_append(payload,
                         sizeof(payload),
                         &length,
                         "{\"deviceId\":\"%s\",\"schemaVersion\":1,"
                         "\"configVersion\":%lu,\"uptimeMs\":%lu",
                         MQTT_DEVICE_ID,
                         (unsigned long)config->config_version,
                         (unsigned long)uptime_ms) != 0)
    {
        tls_log("MQTT: telemetry JSON too large");
        return -1;
    }

    if (((config->enabled_metrics & DEVICE_METRIC_TEMPERATURE) != 0U) &&
        (mqtt_json_append(payload, sizeof(payload), &length,
                          ",\"temperature\":%lu", simulated_temperature) != 0))
    {
        return -1;
    }

    if (((config->enabled_metrics & DEVICE_METRIC_VOLTAGE) != 0U) &&
        (mqtt_json_append(payload, sizeof(payload), &length, ",\"voltage\":3.30") != 0))
    {
        return -1;
    }

    if (((config->enabled_metrics & DEVICE_METRIC_CURRENT) != 0U) &&
        (mqtt_json_append(payload, sizeof(payload), &length, ",\"current\":0.12") != 0))
    {
        return -1;
    }

    if (((config->enabled_metrics & DEVICE_METRIC_SIGNAL) != 0U) &&
        (mqtt_json_append(payload, sizeof(payload), &length, ",\"signal\":100") != 0))
    {
        return -1;
    }

    if (((config->enabled_metrics & DEVICE_METRIC_HUMIDITY) != 0U) &&
        (mqtt_json_append(payload, sizeof(payload), &length,
                          ",\"humidity\":%lu", simulated_humidity) != 0))
    {
        return -1;
    }

    if (mqtt_json_append(payload, sizeof(payload), &length, "}") != 0)
    {
        return -1;
    }

    return mqtt_publish_json(MQTT_TOPIC_TELEMETRY, payload);
}

static const char *json_value_start(const char *json, const char *key)
{
    char pattern[48];
    const char *value;
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    if ((written < 0) || ((size_t)written >= sizeof(pattern)))
    {
        return NULL;
    }

    value = strstr(json, pattern);
    if (value == NULL)
    {
        return NULL;
    }

    value += strlen(pattern);
    while ((*value == ' ') || (*value == '\t') || (*value == '\r') || (*value == '\n'))
    {
        value++;
    }

    if (*value != ':')
    {
        return NULL;
    }

    value++;
    while ((*value == ' ') || (*value == '\t') || (*value == '\r') || (*value == '\n'))
    {
        value++;
    }

    return value;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *value = json_value_start(json, key);
    size_t length = 0U;

    if ((value == NULL) || (*value != '\"') || (out_len == 0U))
    {
        return -1;
    }

    value++;
    while ((value[length] != '\0') && (value[length] != '\"'))
    {
        if ((value[length] == '\\') || ((length + 1U) >= out_len))
        {
            return -1;
        }
        length++;
    }

    if (value[length] != '\"')
    {
        return -1;
    }

    memcpy(out, value, length);
    out[length] = '\0';
    return 0;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
    const char *value = json_value_start(json, key);

    if ((value != NULL) && (strncmp(value, "true", 4U) == 0))
    {
        *out = 1;
        return 0;
    }

    if ((value != NULL) && (strncmp(value, "false", 5U) == 0))
    {
        *out = 0;
        return 0;
    }

    return -1;
}

static int json_get_uint32(const char *json, const char *key, uint32_t *out)
{
    const char *value = json_value_start(json, key);
    char *end;
    unsigned long parsed;

    if ((value == NULL) || (*value == '-') || (*value == '+'))
    {
        return -1;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if ((end == value) || (errno != 0) || (parsed > 0xFFFFFFFFUL))
    {
        return -1;
    }

    while ((*end == ' ') || (*end == '\t') || (*end == '\r') || (*end == '\n'))
    {
        end++;
    }

    if ((*end != ',') && (*end != '}') && (*end != ']'))
    {
        return -1;
    }

    *out = (uint32_t)parsed;
    return 0;
}

static uint8_t mqtt_metric_mask_for_name(const char *name)
{
    size_t index;

    for (index = 0U;
         index < (sizeof(mqtt_metric_definitions) / sizeof(mqtt_metric_definitions[0]));
         index++)
    {
        if (strcmp(name, mqtt_metric_definitions[index].name) == 0)
        {
            return mqtt_metric_definitions[index].mask;
        }
    }

    return 0U;
}

static int json_get_metric_mask(const char *json, const char *key, uint8_t *out)
{
    const char *value = json_value_start(json, key);
    uint8_t mask = 0U;

    if ((value == NULL) || (*value != '['))
    {
        return -1;
    }

    value++;
    for (;;)
    {
        char metric[32];
        size_t length = 0U;
        uint8_t metric_mask;

        while ((*value == ' ') || (*value == '\t') || (*value == '\r') || (*value == '\n'))
        {
            value++;
        }

        if (*value == ']')
        {
            if (mask == 0U)
            {
                return -1;
            }
            *out = mask;
            return 0;
        }

        if (*value != '"')
        {
            return -1;
        }
        value++;

        while ((*value != '\0') && (*value != '"'))
        {
            if ((*value == '\\') || ((length + 1U) >= sizeof(metric)))
            {
                return -1;
            }
            metric[length++] = *value++;
        }

        if (*value != '"')
        {
            return -1;
        }
        value++;
        metric[length] = '\0';

        metric_mask = mqtt_metric_mask_for_name(metric);
        if ((metric_mask == 0U) || ((mask & metric_mask) != 0U))
        {
            return -1;
        }
        mask |= metric_mask;

        while ((*value == ' ') || (*value == '\t') || (*value == '\r') || (*value == '\n'))
        {
            value++;
        }

        if (*value == ',')
        {
            value++;
            continue;
        }

        if (*value != ']')
        {
            return -1;
        }
    }
}

static int device_log_level_from_name(const char *name, uint8_t *level)
{
    uint8_t candidate;

    for (candidate = DEVICE_LOG_LEVEL_ERROR;
         candidate <= DEVICE_LOG_LEVEL_TRACE;
         candidate++)
    {
        if (strcmp(name, DeviceConfig_LogLevelName(candidate)) == 0)
        {
            *level = candidate;
            return 0;
        }
    }

    return -1;
}

static int mqtt_publish_command_result(const char *command_id,
                                       const char *status,
                                       const char *message)
{
    char payload[MQTT_JSON_BUFFER_SIZE];
    int written = snprintf(payload,
                           sizeof(payload),
                           "{\"deviceId\":\"%s\",\"schemaVersion\":1,"
                           "\"commandId\":\"%s\",\"status\":\"%s\","
                           "\"message\":\"%s\",\"uptimeMs\":%lu}",
                           MQTT_DEVICE_ID,
                           command_id,
                           status,
                           message,
                           (unsigned long)HAL_GetTick());

    if ((written < 0) || ((size_t)written >= sizeof(payload)))
    {
        tls_log("MQTT: command result JSON too large");
        return -1;
    }

    return mqtt_publish_json(MQTT_TOPIC_CMD_RESULT, payload);
}

static int mqtt_publish_reported_config(uint32_t request_version,
                                        const char *status,
                                        const char *message)
{
    char payload[MQTT_JSON_BUFFER_SIZE];
    char metrics[128];
    const DeviceConfig *config = DeviceConfig_Get();
    int written;

    if (mqtt_metrics_json(config->enabled_metrics, metrics, sizeof(metrics)) != 0)
    {
        tls_log("MQTT CONFIG: metrics JSON too large");
        return -1;
    }

    written = snprintf(payload,
                       sizeof(payload),
                       "{\"deviceId\":\"%s\",\"schemaVersion\":%u,"
                       "\"requestVersion\":%lu,\"configVersion\":%lu,"
                       "\"status\":\"%s\",\"message\":\"%s\","
                       "\"uptimeMs\":%lu,\"configuration\":{"
                       "\"telemetry\":{\"frequencySeconds\":%lu,"
                       "\"enabledMetrics\":%s},"
                       "\"diagnostics\":{\"logLevel\":\"%s\","
                       "\"maxMessageBytes\":%u},"
                       "\"network\":{\"mqttKeepAliveSeconds\":%u},"
                       "\"firmware\":{\"allowRollback\":%s}}}",
                       MQTT_DEVICE_ID,
                       DEVICE_CONFIG_SCHEMA_VERSION,
                       (unsigned long)request_version,
                       (unsigned long)config->config_version,
                       status,
                       message,
                       (unsigned long)HAL_GetTick(),
                       (unsigned long)config->telemetry_frequency_seconds,
                       metrics,
                       DeviceConfig_LogLevelName(config->log_level),
                       (unsigned int)config->diagnostics_max_message_bytes,
                       (unsigned int)config->mqtt_keep_alive_seconds,
                       config->allow_rollback ? "true" : "false");

    if ((written < 0) || ((size_t)written >= sizeof(payload)))
    {
        tls_log("MQTT CONFIG: reported JSON too large");
        return -1;
    }

    return mqtt_publish_json(MQTT_TOPIC_CONFIG_REPORTED, payload);
}

static int mqtt_reject_config(uint32_t request_version, const char *reason)
{
    tls_logf("MQTT CONFIG: rejected request=%lu reason=%s",
             (unsigned long)request_version,
             reason);
    return mqtt_publish_reported_config(request_version, "REJECTED", reason);
}

static int mqtt_handle_desired_config(const char *payload)
{
    DeviceConfig candidate = *DeviceConfig_Get();
    DeviceConfigApplyResult result;
    uint32_t schema_version;
    uint32_t request_version = candidate.config_version;
    uint32_t number;
    char device_id[96];
    char log_level[16];
    int allow_rollback;

    if (json_get_uint32(payload, "configVersion", &request_version) != 0)
    {
        return mqtt_reject_config(request_version, "Invalid configVersion");
    }

    if ((json_get_uint32(payload, "schemaVersion", &schema_version) != 0) ||
        (schema_version != DEVICE_CONFIG_SCHEMA_VERSION))
    {
        return mqtt_reject_config(request_version, "Unsupported schemaVersion");
    }

    if ((json_get_string(payload, "deviceId", device_id, sizeof(device_id)) != 0) ||
        (strcmp(device_id, MQTT_DEVICE_ID) != 0))
    {
        return mqtt_reject_config(request_version, "deviceId mismatch");
    }

    candidate.config_version = request_version;

    if (json_get_uint32(payload, "frequencySeconds", &number) != 0)
    {
        return mqtt_reject_config(request_version, "Invalid telemetry frequency");
    }
    candidate.telemetry_frequency_seconds = number;

    if (json_get_metric_mask(payload, "enabledMetrics", &candidate.enabled_metrics) != 0)
    {
        return mqtt_reject_config(request_version, "Invalid enabledMetrics");
    }

    if ((json_get_string(payload, "logLevel", log_level, sizeof(log_level)) != 0) ||
        (device_log_level_from_name(log_level, &candidate.log_level) != 0))
    {
        return mqtt_reject_config(request_version, "Invalid logLevel");
    }

    if ((json_get_uint32(payload, "maxMessageBytes", &number) != 0) ||
        (number > 0xFFFFU))
    {
        return mqtt_reject_config(request_version, "Invalid maxMessageBytes");
    }
    candidate.diagnostics_max_message_bytes = (uint16_t)number;

    if ((json_get_uint32(payload, "mqttKeepAliveSeconds", &number) != 0) ||
        (number > 0xFFFFU))
    {
        return mqtt_reject_config(request_version, "Invalid MQTT keep alive");
    }
    candidate.mqtt_keep_alive_seconds = (uint16_t)number;

    if (json_get_bool(payload, "allowRollback", &allow_rollback) != 0)
    {
        return mqtt_reject_config(request_version, "Invalid allowRollback");
    }
    candidate.allow_rollback = allow_rollback ? 1U : 0U;

    result = DeviceConfig_Apply(&candidate);
    switch (result)
    {
        case DEVICE_CONFIG_APPLIED:
            mqtt_last_telemetry_ms = HAL_GetTick();
            tls_logf("MQTT CONFIG: applied version=%lu telemetry=%lus",
                     (unsigned long)candidate.config_version,
                     (unsigned long)candidate.telemetry_frequency_seconds);
            return mqtt_publish_reported_config(request_version,
                                                "APPLIED",
                                                "Configuration applied");

        case DEVICE_CONFIG_UNCHANGED:
            return mqtt_publish_reported_config(request_version,
                                                "APPLIED",
                                                "Configuration already applied");

        case DEVICE_CONFIG_REJECTED_STALE:
            return mqtt_reject_config(request_version, "Stale configVersion");

        case DEVICE_CONFIG_REJECTED_CONFLICT:
            return mqtt_reject_config(request_version, "configVersion conflict");

        case DEVICE_CONFIG_PERSISTENCE_ERROR:
            return mqtt_reject_config(request_version, "Flash persistence failed");

        case DEVICE_CONFIG_REJECTED_INVALID:
        default:
            return mqtt_reject_config(request_version, "Configuration out of range");
    }
}

static int mqtt_handle_command(const char *payload)
{
    char command_id[96];
    char command_type[48];
    int led_on;

    if (json_get_string(payload, "commandId", command_id, sizeof(command_id)) != 0)
    {
        tls_log("MQTT CMD: missing/invalid commandId");
        return mqtt_publish_command_result("unknown", "FAILED", "Invalid commandId");
    }

    if (json_get_string(payload, "type", command_type, sizeof(command_type)) != 0)
    {
        tls_log("MQTT CMD: missing/invalid type");
        return mqtt_publish_command_result(command_id, "FAILED", "Invalid command type");
    }

    if (strcmp(command_type, "set_led") != 0)
    {
        tls_logf("MQTT CMD: unsupported type=%s", command_type);
        return mqtt_publish_command_result(command_id, "FAILED", "Unsupported command");
    }

    if (json_get_bool(payload, "value", &led_on) != 0)
    {
        tls_log("MQTT CMD: set_led requires boolean value");
        return mqtt_publish_command_result(command_id, "FAILED", "Value must be boolean");
    }

    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    tls_logf("MQTT CMD: set_led=%d commandId=%s", led_on, command_id);

    return mqtt_publish_command_result(command_id,
                                       "ACKED",
                                       led_on ? "LED switched on" : "LED switched off");
}

static int mqtt_decode_remaining_length(const uint8_t *packet,
                                        size_t available,
                                        size_t *remaining,
                                        size_t *header_length)
{
    size_t value = 0U;
    size_t multiplier = 1U;
    size_t index;

    for (index = 1U; (index < available) && (index <= 4U); index++)
    {
        uint8_t encoded = packet[index];
        value += (size_t)(encoded & 0x7FU) * multiplier;

        if ((encoded & 0x80U) == 0U)
        {
            *remaining = value;
            *header_length = index + 1U;
            return 1;
        }

        multiplier *= 128U;
    }

    return (index > 4U) ? -1 : 0;
}

static int mqtt_handle_publish_packet(const uint8_t *packet,
                                      size_t packet_length,
                                      size_t header_length)
{
    size_t pos = header_length;
    size_t topic_length;
    size_t payload_length;
    uint8_t qos = (uint8_t)((packet[0] >> 1U) & 0x03U);
    uint16_t packet_id = 0U;
    char topic[160];
    char payload[MQTT_JSON_BUFFER_SIZE];

    if ((pos + 2U) > packet_length)
    {
        return -1;
    }

    topic_length = ((size_t)packet[pos] << 8U) | packet[pos + 1U];
    pos += 2U;

    if ((topic_length == 0U) || (topic_length >= sizeof(topic)) ||
        ((pos + topic_length) > packet_length))
    {
        return -1;
    }

    memcpy(topic, &packet[pos], topic_length);
    topic[topic_length] = '\0';
    pos += topic_length;

    if (qos > 1U)
    {
        return -1;
    }

    if (qos > 0U)
    {
        if ((pos + 2U) > packet_length)
        {
            return -1;
        }
        packet_id = ((uint16_t)packet[pos] << 8U) | packet[pos + 1U];
        pos += 2U;
    }

    payload_length = packet_length - pos;
    if (payload_length >= sizeof(payload))
    {
        tls_log("MQTT: incoming payload too large");
        return -1;
    }

    memcpy(payload, &packet[pos], payload_length);
    payload[payload_length] = '\0';
    tls_logf("MQTT: received PUBLISH %s", topic);

    if (strcmp(topic, MQTT_TOPIC_CMD) == 0)
    {
        if (mqtt_handle_command(payload) != 0)
        {
            return -1;
        }
    }
    else if (strcmp(topic, MQTT_TOPIC_CONFIG_DESIRED) == 0)
    {
        if (mqtt_handle_desired_config(payload) != 0)
        {
            return -1;
        }
    }

    if (qos == 1U)
    {
        uint8_t puback[4] = {
            0x40U,
            0x02U,
            (uint8_t)((packet_id >> 8U) & 0xFFU),
            (uint8_t)(packet_id & 0xFFU)
        };

        if (tls_write_all(puback, sizeof(puback), 5000U) != 0)
        {
            return -1;
        }
    }

    return 0;
}

static int mqtt_process_rx_buffer(void)
{
    while (mqtt_rx_length > 0U)
    {
        size_t remaining = 0U;
        size_t header_length = 0U;
        size_t packet_length;
        int decoded = mqtt_decode_remaining_length(mqtt_rx_buffer,
                                                   mqtt_rx_length,
                                                   &remaining,
                                                   &header_length);

        if (decoded == 0)
        {
            return 0;
        }

        if (decoded < 0)
        {
            tls_log("MQTT: invalid remaining length");
            return -1;
        }

        packet_length = header_length + remaining;
        if (packet_length > sizeof(mqtt_rx_buffer))
        {
            tls_log("MQTT: packet exceeds RX buffer");
            return -1;
        }

        if (mqtt_rx_length < packet_length)
        {
            return 0;
        }

        if ((mqtt_rx_buffer[0] & 0xF0U) == 0x30U)
        {
            if (mqtt_handle_publish_packet(mqtt_rx_buffer,
                                           packet_length,
                                           header_length) != 0)
            {
                return -1;
            }
        }

        mqtt_rx_length -= packet_length;
        if (mqtt_rx_length > 0U)
        {
            memmove(mqtt_rx_buffer,
                    &mqtt_rx_buffer[packet_length],
                    mqtt_rx_length);
        }
    }

    return 0;
}

static int mqtt_read_available(void)
{
    for (;;)
    {
        int ret;

        if (mqtt_rx_length == sizeof(mqtt_rx_buffer))
        {
            tls_log("MQTT: RX buffer full");
            return -1;
        }

        ret = mbedtls_ssl_read(&tls_ssl,
                               &mqtt_rx_buffer[mqtt_rx_length],
                               sizeof(mqtt_rx_buffer) - mqtt_rx_length);

        if (ret > 0)
        {
            mqtt_rx_length += (size_t)ret;
            if (mqtt_process_rx_buffer() != 0)
            {
                return -1;
            }
            continue;
        }

        if ((ret == MBEDTLS_ERR_SSL_WANT_READ) ||
            (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
        {
            return 0;
        }

        if (ret == 0)
        {
            tls_log("MQTT: broker closed connection");
            return -1;
        }

        tls_log_mbedtls_error("MQTT read", ret);
        return -1;
    }
}

static int mqtt_send_ping(void)
{
    static const uint8_t pingreq[2] = { 0xC0U, 0x00U };
    return tls_write_all(pingreq, sizeof(pingreq), 5000U);
}

int TlsClient_MqttTest(void)
{
    if (!tls_connected)
    {
        tls_log("MQTT: refused, TLS not connected");
        return -1;
    }

    tls_log("MQTT: test start");

    if (mqtt_send_connect() != 0)
    {
        tls_log("MQTT: CONNECT failed");
        return -1;
    }

    if (mqtt_subscribe_topic(MQTT_TOPIC_CMD, 1U) != 0)
    {
        tls_log("MQTT: command SUBSCRIBE failed");
        return -1;
    }

    if (mqtt_subscribe_topic(MQTT_TOPIC_CONFIG_DESIRED, 2U) != 0)
    {
        tls_log("MQTT: configuration SUBSCRIBE failed");
        return -1;
    }

    if (mqtt_publish_status() != 0)
    {
        tls_log("MQTT: PUBLISH failed");
        return -1;
    }

    if (mqtt_publish_reported_config(DeviceConfig_Get()->config_version,
                                     "APPLIED",
                                     "Configuration restored after boot") != 0)
    {
        tls_log("MQTT: initial reported config failed");
        return -1;
    }

    if (mqtt_publish_telemetry() != 0)
    {
        tls_log("MQTT: initial telemetry failed");
        return -1;
    }

    mqtt_ready = 1;
    mqtt_rx_length = 0U;
    mqtt_last_telemetry_ms = HAL_GetTick();
    mqtt_last_ping_ms = HAL_GetTick();
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

    tls_log("MQTT: test OK");
    return 0;
}

int TlsClient_MqttProcess(void)
{
    const DeviceConfig *config;
    uint32_t now;
    uint32_t telemetry_interval_ms;
    uint32_t ping_interval_ms;

    if (!tls_connected || !mqtt_ready)
    {
        return -1;
    }

    if (mqtt_read_available() != 0)
    {
        mqtt_ready = 0;
        return -1;
    }

    config = DeviceConfig_Get();
    telemetry_interval_ms = config->telemetry_frequency_seconds * 1000U;
    ping_interval_ms = (uint32_t)mqtt_negotiated_keep_alive_seconds * 500U;

    now = HAL_GetTick();
    if ((now - mqtt_last_telemetry_ms) >= telemetry_interval_ms)
    {
        mqtt_last_telemetry_ms = now;
        if (mqtt_publish_telemetry() != 0)
        {
            mqtt_ready = 0;
            return -1;
        }
    }

    if ((now - mqtt_last_ping_ms) >= ping_interval_ms)
    {
        mqtt_last_ping_ms = now;
        if (mqtt_send_ping() != 0)
        {
            mqtt_ready = 0;
            return -1;
        }
    }

    return 0;
}

int TlsClient_MqttIsReady(void)
{
    return mqtt_ready;
}

void TlsClient_Disconnect(void)
{
    tls_log("TLS: disconnect");
    mqtt_ready = 0;
    mqtt_rx_length = 0U;
    tls_ctx_free_all();
}
