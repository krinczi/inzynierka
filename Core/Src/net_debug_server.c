/*
 * net_debug_server.c
 *
 *  Created on: Apr 11, 2026
 *      Author: justy
 */


#include "net_debug_server.h"

#include "main.h"
#include "lwip.h"
#include "debug_logger.h"
#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"

extern struct netif gnetif;

static uint32_t last_net_ms = 0;
static uint8_t dhcp_started = 0;
static uint8_t tcp_server_started = 0;

static uint8_t prev_link_up = 0xFF;
static uint8_t prev_netif_up = 0xFF;
static uint8_t prev_dhcp_ok = 0xFF;
#define NETDBG_LOG(text)      Debug_LogLine(DEBUG_SRC_NET, (text))
#define NETDBG_LOGF(...)      Debug_Logf(DEBUG_SRC_NET, __VA_ARGS__)
#define TCPDBG_LOG(text)      Debug_LogLine(DEBUG_SRC_TCP, (text))
#define TCPDBG_LOGF(...)      Debug_Logf(DEBUG_SRC_TCP, __VA_ARGS__)
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    char rx[128];
    const char *reply;
    err_t wr_err;
    u16_t copy_len;

    (void)arg;

    TCPDBG_LOGF("TCP: recv p=%p len=%d err=%d",
                (void *)p, (p ? p->tot_len : 0), (int)err);

    if (p == NULL)
    {
    	TCPDBG_LOG("TCP: client closed");
    	tcp_close(tpcb);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    copy_len = (p->tot_len < sizeof(rx) - 1U) ? p->tot_len : (sizeof(rx) - 1U);
    pbuf_copy_partial(p, rx, copy_len, 0);
    rx[copy_len] = '\0';

    TCPDBG_LOGF("TCP: payload='%s'", rx);

    if ((strcmp(rx, "meow") == 0) ||
        (strcmp(rx, "meow\r\n") == 0) ||
        (strcmp(rx, "meow\n") == 0))
    {
        reply = "Stm says meoooow\r\n";
    }
    else
    {
        reply = "not meow\r\n";
    }

    wr_err = tcp_write(tpcb, reply, strlen(reply), TCP_WRITE_FLAG_COPY);
    if (wr_err == ERR_OK)
    {
        tcp_output(tpcb);
        TCPDBG_LOG("TCP: reply sent");
    }
    else
    {
    	TCPDBG_LOGF("TCP: tcp_write failed err=%d", (int)wr_err);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    err_t wr_err;
    const char hello[] = "CONNECTED\r\n";

    (void)arg;

    TCPDBG_LOGF("TCP: client accepted err=%d", (int)err);

    tcp_recv(newpcb, tcp_server_recv);

    wr_err = tcp_write(newpcb, hello, sizeof(hello) - 1U, TCP_WRITE_FLAG_COPY);
    if (wr_err == ERR_OK)
    {
        tcp_output(newpcb);
        TCPDBG_LOG("TCP: welcome sent");
    }
    else
    {
    	TCPDBG_LOGF("TCP: welcome write failed err=%d", (int)wr_err);
    }

    return ERR_OK;
}

static void tcp_server_init(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb == NULL)
    {
    	TCPDBG_LOG("TCP: tcp_new failed");
    	return;
    }

    if (tcp_bind(pcb, IP_ADDR_ANY, 5000) != ERR_OK)
    {
    	TCPDBG_LOG("TCP: bind failed");
    	tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (pcb == NULL)
    {
    	TCPDBG_LOG("TCP: bind failed");
    	return;
    }

    tcp_accept(pcb, tcp_server_accept);
    TCPDBG_LOG("TCP: server listening on port 5000");
}

static void net_log_status(void)
{
    char ip_buf[16];
    char mask_buf[16];
    char gw_buf[16];

    uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
    uint8_t netif_up = netif_is_up(&gnetif) ? 1U : 0U;
    uint8_t dhcp_ok = 0U;

#if LWIP_DHCP
    dhcp_ok = dhcp_supplied_address(&gnetif) ? 1U : 0U;
#endif

    ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip_buf, sizeof(ip_buf));
    ip4addr_ntoa_r(netif_ip4_netmask(&gnetif), mask_buf, sizeof(mask_buf));
    ip4addr_ntoa_r(netif_ip4_gw(&gnetif), gw_buf, sizeof(gw_buf));

    NETDBG_LOGF("NET: link=%s netif=%s dhcp=%s ip=%s mask=%s gw=%s",
                link_up ? "UP" : "DOWN",
                netif_up ? "UP" : "DOWN",
                dhcp_ok ? "YES" : "NO",
                ip_buf,
                mask_buf,
                gw_buf);
}

static void net_log_on_change(void)
{
    uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
    uint8_t netif_up = netif_is_up(&gnetif) ? 1U : 0U;
    uint8_t dhcp_ok = 0U;

#if LWIP_DHCP
    dhcp_ok = dhcp_supplied_address(&gnetif) ? 1U : 0U;
#endif

    if ((link_up != prev_link_up) ||
        (netif_up != prev_netif_up) ||
        (dhcp_ok != prev_dhcp_ok))
    {
        prev_link_up = link_up;
        prev_netif_up = netif_up;
        prev_dhcp_ok = dhcp_ok;
        net_log_status();
    }
}

static void dhcp_try_start(void)
{
#if LWIP_DHCP
    uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
    uint8_t netif_up = netif_is_up(&gnetif) ? 1U : 0U;

    if (link_up && netif_up && !dhcp_started)
    {
        err_t err = dhcp_start(&gnetif);
        NETDBG_LOGF("DHCP: start err=%d", (int)err);

        if (err == ERR_OK)
        {
            dhcp_started = 1U;
        }
    }

    if (!link_up && dhcp_started)
    {
        dhcp_stop(&gnetif);
        dhcp_started = 0U;
        NETDBG_LOG("DHCP: stopped");
    }
#endif
}

void NetDebugServer_Init(void)
{
    NETDBG_LOG("NETDBG: init");
    dhcp_try_start();
    net_log_status();
}

void NetDebugServer_Process(void)
{
    dhcp_try_start();

    if (!tcp_server_started &&
        netif_is_link_up(&gnetif) &&
        netif_is_up(&gnetif) &&
        dhcp_supplied_address(&gnetif))
    {
        tcp_server_init();
        tcp_server_started = 1U;
    }

    net_log_on_change();

    if ((HAL_GetTick() - last_net_ms) >= 2000U)
    {
        last_net_ms = HAL_GetTick();
        net_log_status();
    }
}
