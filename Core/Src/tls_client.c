#include "tls_client.h"
#include "aws_iot_config.h"
#include "aws_certs.h"
#include "main.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
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
#include <errno.h>
#include "lwip/ip_addr.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
extern UART_HandleTypeDef huart3;

extern volatile uint32_t eth_rx_irq_count;
extern volatile uint32_t eth_input_pbuf_count;
extern volatile uint32_t eth_err_irq_count;
extern volatile uint32_t eth_last_dma_error;
extern volatile uint32_t eth_last_hal_error;
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
static volatile int tls_dns_done = 0;
static volatile err_t tls_dns_err = ERR_INPROGRESS;
static ip_addr_t tls_dns_addr;

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
//static const int aws_ciphersuites[] = {
//    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
//    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
//    0
//};
static void tls_log(const char *msg)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
    HAL_UART_Transmit(&huart3, (uint8_t *)"\r\n", 2, HAL_MAX_DELAY);
}

static void tls_logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tls_log(buf);
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
	tls_log("CTX: manual net init");
	tls_net.fd = -1;
	tls_log("CTX: manual net init OK");

	tls_log("CTX: ssl init");
	mbedtls_ssl_init(&tls_ssl);
    tls_log("CTX: ssl config init");
    mbedtls_ssl_config_init(&tls_conf);

    tls_log("CTX: ca cert init");
    mbedtls_x509_crt_init(&tls_ca_cert);

    tls_log("CTX: client cert init");
    mbedtls_x509_crt_init(&tls_client_cert);

    tls_log("CTX: pk init");
    mbedtls_pk_init(&tls_pkey);

    tls_log("CTX: ctr drbg init");
    mbedtls_ctr_drbg_init(&tls_ctr_drbg);

    tls_log("CTX: entropy init");
    mbedtls_entropy_init(&tls_entropy);

    tls_log("CTX: all init OK");

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
    tls_log("TLS: init");
    tls_connected = 0;
    tls_ctx_ready = 0;
    return 0;
}
static int Tls_ResolveHost(const char *host,
                           char *ipbuf,
                           size_t ipbuf_len,
                           uint32_t timeout_ms)
{
    ip_addr_t addr;

    tls_logf("TLS DNS: resolve %s", host);
    tls_logf("TLS DNS: server0=%s", ipaddr_ntoa(dns_getserver(0)));

    tls_dns_done = 0;
    tls_dns_err = ERR_INPROGRESS;
    memset(&tls_dns_addr, 0, sizeof(tls_dns_addr));

    tls_log("TLS DNS: before dns_gethostbyname");

    err_t err = dns_gethostbyname(host, &addr, Tls_DnsFound, NULL);

    tls_logf("TLS DNS: dns_gethostbyname start err=%d", (int)err);

    if (err == ERR_OK)
    {
        ipaddr_ntoa_r(&addr, ipbuf, ipbuf_len);
        tls_logf("TLS DNS: resolved cached -> %s", ipbuf);
        return 0;
    }

    if (err != ERR_INPROGRESS)
    {
        tls_logf("TLS DNS: start failed err=%d", (int)err);
        return -1;
    }

    uint32_t start = HAL_GetTick();

    while (!tls_dns_done)
    {
        if ((HAL_GetTick() - start) > timeout_ms)
        {
            tls_log("TLS DNS: TIMEOUT");
            return -1;
        }

        osDelay(100);
    }

    tls_logf("TLS DNS: callback err=%d", (int)tls_dns_err);

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
                             uint32_t timeout_ms);
static int Tls_LwipConnectHost(mbedtls_net_context *ctx,
                               const char *host,
                               uint16_t port,
                               uint32_t timeout_ms)
{
    char ipbuf[48];

    if (Tls_ResolveHost(host, ipbuf, sizeof(ipbuf), 15000) != 0)
    {
        tls_log("TLS: hostname resolve failed");
        return -10;
    }

    return Tls_LwipConnectIp(ctx, ipbuf, port, timeout_ms);
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

    tls_log("TLS CONN: before lwip_socket()");
    sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    tls_logf("TLS CONN: after lwip_socket() sock=%d errno=%d", sock, errno);

    if (sock < 0)
    {
        tls_logf("TLS: socket failed=%d errno=%d", sock, errno);
        return -1;
    }

    int nonblock = 1;

    tls_log("TLS CONN: before ioctl nonblock");
    ret = lwip_ioctl(sock, FIONBIO, &nonblock);
    tls_logf("TLS CONN: after ioctl ret=%d errno=%d", ret, errno);

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
    tls_log("TLS CONN: before lwip_connect()");
    ret = lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    tls_logf("TLS CONN: after lwip_connect ret=%d errno=%d", ret, errno);

    if (ret != 0)
    {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        errno = 0;
        tls_logf("TLS CONN: before lwip_select timeout=%lu ms", timeout_ms);
        ret = lwip_select(sock + 1, NULL, &wfds, NULL, &tv);
        tls_logf("TLS CONN: after lwip_select ret=%d errno=%d", ret, errno);

        if (ret <= 0)
        {
            tls_logf("TLS: connect timeout/select ret=%d errno=%d", ret, errno);
            lwip_close(sock);
            return -3;
        }

        int err = 0;
        socklen_t len = sizeof(err);

        errno = 0;
        tls_log("TLS CONN: before lwip_getsockopt");
        ret = lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
        tls_logf("TLS CONN: after lwip_getsockopt ret=%d err=%d errno=%d", ret, err, errno);

        if (ret != 0 || err != 0)
        {
            tls_logf("TLS: connect socket error ret=%d err=%d errno=%d", ret, err, errno);
            lwip_close(sock);
            return -4;
        }
    }

    ctx->fd = sock;

    struct timeval rcv_to;
    rcv_to.tv_sec = 5;
    rcv_to.tv_usec = 0;

    struct timeval snd_to;
    snd_to.tv_sec = 5;
    snd_to.tv_usec = 0;

    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

    tls_logf("TLS: lwip socket connected fd=%d nonblock+timeout OK", sock);
    return 0;
}
static int Tls_LwipSend(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;

    tls_send_calls++;

    tls_logf("TLS IO: send enter call=%lu len=%lu fd=%d",
             tls_send_calls,
             (uint32_t)len,
             fd);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    errno = 0;
    int sel = lwip_select(fd + 1, NULL, &wfds, NULL, &tv);

    tls_logf("TLS IO: send select ret=%d errno=%d", sel, errno);

    if (sel == 0)
    {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    if (sel < 0)
    {
        tls_logf("TLS SEND: select failed errno=%d", errno);
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    size_t to_send = len;

    if (to_send > 1024)
    {
        to_send = 1024;
    }

    tls_logf("TLS IO: before lwip_send to_send=%lu", (uint32_t)to_send);

    errno = 0;
    int ret = lwip_send(fd, buf, to_send, 0);

    tls_logf("TLS IO: after lwip_send ret=%d errno=%d", ret, errno);

    if (ret > 0)
    {
        tls_logf("TLS IO: send OK ret=%d rx_irq=%lu pbuf=%lu err_irq=%lu",
                 ret,
                 eth_rx_irq_count,
                 eth_input_pbuf_count,
                 eth_err_irq_count);

        return ret;
    }

    if (ret == 0)
    {
        tls_log("TLS SEND: ret=0");
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS)
    {
        tls_logf("TLS SEND: wouldblock errno=%d", errno);
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    tls_logf("TLS SEND: failed ret=%d errno=%d", ret, errno);
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int Tls_LwipRecv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = ((mbedtls_net_context *)ctx)->fd;

    tls_recv_calls++;

    if (tls_recv_calls <= 10 || (tls_recv_calls % 50) == 0)
    {
        tls_logf("TLS IO: recv enter call=%lu len=%lu",
                 tls_recv_calls,
                 (uint32_t)len);
    }

    errno = 0;

    /*
     * Socket jest już ustawiony jako non-blocking przez FIONBIO
     * w Tls_LwipConnectIp(), więc recv nie powinien wisieć.
     */
    int ret = lwip_recv(fd, buf, len, 0);

    if (ret > 0)
    {
        if (tls_recv_calls <= 10 || (tls_recv_calls % 50) == 0)
        {
            tls_logf("TLS IO: recv exit call=%lu ret=%d errno=%d",
                     tls_recv_calls,
                     ret,
                     errno);
        }

        return ret;
    }

    if (ret == 0)
    {
        tls_log("TLS RECV: connection closed");
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS)
    {
        if (tls_recv_calls <= 10 || (tls_recv_calls % 50) == 0)
        {
        	tls_logf("TLS RECV: wb call=%lu err=%d rx=%lu pbuf=%lu",
        	         tls_recv_calls,
        	         errno,
        	         eth_rx_irq_count,
        	         eth_input_pbuf_count);
        }

        return MBEDTLS_ERR_SSL_WANT_READ;
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

    tls_log("TLS: free ctx...");
    tls_ctx_free_all();
    tls_log("TLS: free ctx OK");

    tls_log("TLS: init ctx...");
    tls_ctx_init_all();
    tls_log("TLS: init ctx OK");

    tls_log("TLS: seeding RNG...");
    ret = mbedtls_ctr_drbg_seed(&tls_ctr_drbg,
                                mbedtls_entropy_func,
                                &tls_entropy,
                                (const unsigned char *)pers,
                                strlen(pers));
    tls_log("TLS: seed returned");

    if (ret != 0)
    {
        tls_log_mbedtls_error("mbedtls_ctr_drbg_seed", ret);
        goto fail;
    }

    tls_log("TLS: RNG seed OK");

    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)AWS_IOT_PORT);

    tls_logf("TLS: resolve AWS HOST %s", AWS_IOT_ENDPOINT);

    ret = Tls_ResolveHost(AWS_IOT_ENDPOINT, aws_ip, sizeof(aws_ip), 15000);
    if (ret != 0)
    {
        tls_logf("TLS: host resolve failed ret=%d", ret);
        goto fail;
    }

    tls_logf("TLS: AWS resolved cached IP=%s", aws_ip);

    ret = mbedtls_x509_crt_parse(&tls_ca_cert,
                                 (const unsigned char *)AWS_ROOT_CA_PEM,
                                 sizeof(AWS_ROOT_CA_PEM));
    if (ret < 0)
    {
        tls_log_mbedtls_error("mbedtls_x509_crt_parse(root_ca)", ret);
        goto fail;
    }

    tls_log("TLS: Root CA parsed");

    ret = mbedtls_x509_crt_parse(&tls_client_cert,
                                 (const unsigned char *)AWS_DEVICE_CERT_PEM,
                                 sizeof(AWS_DEVICE_CERT_PEM));
    if (ret < 0)
    {
        tls_log_mbedtls_error("mbedtls_x509_crt_parse(device_cert)", ret);
        goto fail;
    }

    tls_log("TLS: device cert parsed");

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

    tls_log("TLS: private key parsed");
    tls_logf("TLS: TCP connect AWS IP %s:%s", aws_ip, port_str);

    ret = -1;

    for (int attempt = 1; attempt <= 3; attempt++)
    {
        tls_logf("TLS: TCP connect attempt %d/3", attempt);

        ret = Tls_LwipConnectIp(&tls_net, aws_ip, AWS_IOT_PORT, 60000);

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

//    mbedtls_ssl_conf_ciphersuites(&tls_conf, aws_ciphersuites);

    mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
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
    tls_logf("TLS: hostname/SNI set to %s", AWS_IOT_ENDPOINT);
    tls_log("TLS: set bio custom lwip");
    mbedtls_ssl_set_bio(&tls_ssl, &tls_net, Tls_LwipSend, Tls_LwipRecv, NULL);

    tls_log("TLS: handshake...");

    uint32_t hs_start = HAL_GetTick();
    uint32_t hs_last_log = HAL_GetTick();
    int hs_try = 0;

    do
    {
        uint32_t call_start = HAL_GetTick();

        hs_try++;
        tls_logf("TLS: handshake call try=%d", hs_try);

        ret = mbedtls_ssl_handshake(&tls_ssl);

        uint32_t call_time = HAL_GetTick() - call_start;

        tls_logf("TLS: handshake returned try=%d ret=%d ret_hex=-0x%04X time=%lu ms",
                 hs_try,
                 ret,
                 (ret < 0) ? (unsigned int)(-ret) : 0,
                 (unsigned long)call_time);

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            uint32_t now = HAL_GetTick();

            tls_logf("TLS WAIT: t=%lu rx=%lu pbuf=%lu err=%lu",
                     (unsigned long)(now - hs_start),
                     eth_rx_irq_count,
                     eth_input_pbuf_count,
                     eth_err_irq_count);

            tls_logf("TLS WAIT: stack free words=%lu",
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            tls_logf("TLS WAIT: heap free=%lu min_ever=%lu",
                     (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize());
            if ((now - hs_start) >= 60000)
            {
                tls_log("TLS: handshake TIMEOUT");
                goto fail;
            }

            tls_log("TLS WAIT: before vTaskDelay");
            vTaskDelay(pdMS_TO_TICKS(100));
            tls_log("TLS WAIT: after vTaskDelay");
        }
    }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

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

        /*
         * Na czas diagnostyki NIE robimy goto fail,
         * żeby zobaczyć, czy handshake technicznie przeszedł.
         * Potem wrócimy do twardej weryfikacji.
         */
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

void TlsClient_Disconnect(void)
{
    tls_log("TLS: disconnect");
    tls_ctx_free_all();
}
