/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : This file overrides LwIP stack default configuration
  *                      done in opt.h file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion --------------------------------------*/
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

/*-----------------------------------------------------------------------------*/
/* Current version of LwIP supported by CubeMx: 2.1.2 -*/
/*-----------------------------------------------------------------------------*/

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

#ifdef __cplusplus
 extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- WITH_RTOS enabled (Since FREERTOS is set) -----*/
#define WITH_RTOS 1
/*----- WITH_MBEDTLS enabled (Since MBEDTLS and FREERTOS are set) -----*/
#define WITH_MBEDTLS 1
/*----- CHECKSUM_BY_HARDWARE enabled -----*/
#define CHECKSUM_BY_HARDWARE 0
/*-----------------------------------------------------------------------------*/

/* LwIP Stack Parameters (modified compared to initialization value in opt.h) -*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- Value in opt.h for LWIP_DHCP: 0 -----*/
#define LWIP_DHCP 1
/*----- Default value in ETH configuration GUI in CubeMx: 1524 -----*/
#define ETH_RX_BUFFER_SIZE 1536
/*----- Value in opt.h for LWIP_DNS: 0 -----*/
#define LWIP_DNS 1
/*----- Value in opt.h for MEM_ALIGNMENT: 1 -----*/
#define MEM_ALIGNMENT 4
/*----- Default Value for H7 devices: 0x30044000 -----*/
#define LWIP_RAM_HEAP_POINTER 0x30010000
/*----- Value supported for H7 devices: 1 -----*/
#define LWIP_SUPPORT_CUSTOM_PBUF 1
/*----- Value in opt.h for LWIP_ETHERNET: LWIP_ARP || PPPOE_SUPPORT -*/
#define LWIP_ETHERNET 1
/*----- Value in opt.h for LWIP_DNS_SECURE: (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT) -*/
#define LWIP_DNS_SECURE 0
/*----- Value in opt.h for TCP_SND_QUEUELEN: (4*TCP_SND_BUF + (TCP_MSS - 1))/TCP_MSS -----*/
#define TCP_SND_QUEUELEN 9
/*----- Value in opt.h for TCP_SNDLOWAT: LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (TCP_SND_BUF) - 1) -*/
#define TCP_SNDLOWAT 1071
/*----- Value in opt.h for TCP_SNDQUEUELOWAT: LWIP_MAX(TCP_SND_QUEUELEN)/2, 5) -*/
#define TCP_SNDQUEUELOWAT 5
/*----- Value in opt.h for TCP_WND_UPDATE_THRESHOLD: LWIP_MIN(TCP_WND/4, TCP_MSS*4) -----*/
#define TCP_WND_UPDATE_THRESHOLD 536
/*----- Value in opt.h for LWIP_NETIF_LINK_CALLBACK: 0 -----*/
#define LWIP_NETIF_LINK_CALLBACK 1
/*----- Value in opt.h for TCPIP_THREAD_STACKSIZE: 0 -----*/
#define TCPIP_THREAD_STACKSIZE 2048
/*----- Value in opt.h for TCPIP_THREAD_PRIO: 1 -----*/
#define TCPIP_THREAD_PRIO 24
/*----- Value in opt.h for TCPIP_MBOX_SIZE: 0 -----*/
#define TCPIP_MBOX_SIZE 16
/*----- Value in opt.h for SLIPIF_THREAD_STACKSIZE: 0 -----*/
#define SLIPIF_THREAD_STACKSIZE 1024
/*----- Value in opt.h for SLIPIF_THREAD_PRIO: 1 -----*/
#define SLIPIF_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_THREAD_STACKSIZE: 0 -----*/
#define DEFAULT_THREAD_STACKSIZE 2048
/*----- Value in opt.h for DEFAULT_THREAD_PRIO: 1 -----*/
#define DEFAULT_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_UDP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_UDP_RECVMBOX_SIZE 16
/*----- Value in opt.h for DEFAULT_TCP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_TCP_RECVMBOX_SIZE 16
/*----- Value in opt.h for DEFAULT_ACCEPTMBOX_SIZE: 0 -----*/
#define DEFAULT_ACCEPTMBOX_SIZE 16
/*----- Value in opt.h for RECV_BUFSIZE_DEFAULT: INT_MAX -----*/
#define RECV_BUFSIZE_DEFAULT 2000000000
/*----- Value in opt.h for LWIP_USE_EXTERNAL_MBEDTLS: 0 -----*/
#define LWIP_USE_EXTERNAL_MBEDTLS 1
/*----- Value in opt.h for LWIP_STATS: 1 -----*/
#define LWIP_STATS 0
/*----- Value in opt.h for CHECKSUM_GEN_IP: 1 -----*/
#define CHECKSUM_GEN_IP 1
/*----- Value in opt.h for CHECKSUM_GEN_UDP: 1 -----*/
#define CHECKSUM_GEN_UDP 1
/*----- Value in opt.h for CHECKSUM_GEN_TCP: 1 -----*/
#define CHECKSUM_GEN_TCP 1
/*----- Value in opt.h for CHECKSUM_GEN_ICMP6: 1 -----*/
#define CHECKSUM_GEN_ICMP6 1
/*----- Value in opt.h for CHECKSUM_CHECK_IP: 1 -----*/
#define CHECKSUM_CHECK_IP 0
/*----- Value in opt.h for CHECKSUM_CHECK_UDP: 1 -----*/
#define CHECKSUM_CHECK_UDP 0
/*----- Value in opt.h for CHECKSUM_CHECK_TCP: 1 -----*/
#define CHECKSUM_CHECK_TCP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP6: 1 -----*/
#define CHECKSUM_CHECK_ICMP6 1
/*-----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */
 /* RTOS + sockets */
 #undef NO_SYS
 #define NO_SYS 0

 #undef LWIP_NETCONN
 #define LWIP_NETCONN 1

 #undef LWIP_SOCKET
 #define LWIP_SOCKET 1

 #undef LWIP_COMPAT_SOCKETS
 #define LWIP_COMPAT_SOCKETS 1

 #undef LWIP_SO_RCVTIMEO
 #define LWIP_SO_RCVTIMEO 1

 #undef LWIP_SO_SNDTIMEO
 #define LWIP_SO_SNDTIMEO 1

 /* IPv4 only */
 #undef LWIP_IPV4
 #define LWIP_IPV4 1

 #undef LWIP_IPV6
 #define LWIP_IPV6 0

 /* DNS */
 #undef LWIP_DNS
 #define LWIP_DNS 1

 #undef DNS_MAX_SERVERS
 #define DNS_MAX_SERVERS 2

 #undef DNS_TABLE_SIZE
 #define DNS_TABLE_SIZE 8

 #undef DNS_MAX_NAME_LENGTH
 #define DNS_MAX_NAME_LENGTH 256

 /* Memory */
 #undef MEM_SIZE
 #define MEM_SIZE (64 * 1024)

 #undef MEMP_NUM_TCP_PCB
 #define MEMP_NUM_TCP_PCB 10

 #undef MEMP_NUM_TCP_PCB_LISTEN
 #define MEMP_NUM_TCP_PCB_LISTEN 4

 #undef MEMP_NUM_UDP_PCB
 #define MEMP_NUM_UDP_PCB 8

 #undef MEMP_NUM_NETCONN
 #define MEMP_NUM_NETCONN 16

 #undef MEMP_NUM_TCP_SEG
 #define MEMP_NUM_TCP_SEG 64

 /* Ethernet buffers */
 #undef PBUF_POOL_SIZE
 #define PBUF_POOL_SIZE 24

 #undef PBUF_POOL_BUFSIZE
 #define PBUF_POOL_BUFSIZE 1536

 /* TCP tuning for TLS */
 #undef TCP_MSS
 #define TCP_MSS 1460

 #undef TCP_WND
 #define TCP_WND (8 * TCP_MSS)

 #undef TCP_SND_BUF
 #define TCP_SND_BUF (8 * TCP_MSS)

 #undef TCP_SND_QUEUELEN
 #define TCP_SND_QUEUELEN 32

 #undef TCP_SNDLOWAT
 #define TCP_SNDLOWAT (TCP_SND_BUF / 2)

 #undef TCP_SNDQUEUELOWAT
 #define TCP_SNDQUEUELOWAT 8

 #undef TCP_WND_UPDATE_THRESHOLD
 #define TCP_WND_UPDATE_THRESHOLD (TCP_WND / 4)

 /* Thread stack sizes */
 #undef TCPIP_THREAD_STACKSIZE
 #define TCPIP_THREAD_STACKSIZE 4096

 #undef DEFAULT_THREAD_STACKSIZE
 #define DEFAULT_THREAD_STACKSIZE 4096

 #undef TCPIP_MBOX_SIZE
 #define TCPIP_MBOX_SIZE 32

 #undef DEFAULT_TCP_RECVMBOX_SIZE
 #define DEFAULT_TCP_RECVMBOX_SIZE 32

 #undef DEFAULT_UDP_RECVMBOX_SIZE
 #define DEFAULT_UDP_RECVMBOX_SIZE 16

 #undef DEFAULT_ACCEPTMBOX_SIZE
 #define DEFAULT_ACCEPTMBOX_SIZE 16

 /* Ethernet / ARP / ICMP */
 #undef LWIP_ETHERNET
 #define LWIP_ETHERNET 1

 #undef LWIP_ARP
 #define LWIP_ARP 1

 #undef LWIP_ICMP
 #define LWIP_ICMP 1

 #undef LWIP_RAW
 #define LWIP_RAW 1

 /* Software checksums because CHECKSUM_BY_HARDWARE = 0 */
 #undef CHECKSUM_GEN_IP
 #define CHECKSUM_GEN_IP 1

 #undef CHECKSUM_GEN_UDP
 #define CHECKSUM_GEN_UDP 1

 #undef CHECKSUM_GEN_TCP
 #define CHECKSUM_GEN_TCP 1

 #undef CHECKSUM_GEN_ICMP
 #define CHECKSUM_GEN_ICMP 1

 /*
  * Na czas debugowania możesz zostawić CHECK = 0.
  * Docelowo lepiej dać 1, ale teraz nie mieszajmy dwóch problemów naraz.
  */
 #undef CHECKSUM_CHECK_IP
 #define CHECKSUM_CHECK_IP 0

 #undef CHECKSUM_CHECK_UDP
 #define CHECKSUM_CHECK_UDP 0

 #undef CHECKSUM_CHECK_TCP
 #define CHECKSUM_CHECK_TCP 0

 #undef CHECKSUM_CHECK_ICMP
 #define CHECKSUM_CHECK_ICMP 0

 /* Debug callbacks */
 #undef LWIP_NETIF_STATUS_CALLBACK
 #define LWIP_NETIF_STATUS_CALLBACK 1

 #undef LWIP_NETIF_LINK_CALLBACK
 #define LWIP_NETIF_LINK_CALLBACK 1
#undef MEM_SIZE
#define MEM_SIZE (96 * 1024)

#undef MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB 10

#undef MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN 16

#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG 96

#undef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE 32

#undef TCP_SND_QUEUELEN
#define TCP_SND_QUEUELEN 32

#undef TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE 8192

#undef DEFAULT_THREAD_STACKSIZE
#define DEFAULT_THREAD_STACKSIZE 4096

#undef TCPIP_MBOX_SIZE
#define TCPIP_MBOX_SIZE 32

#undef DEFAULT_TCP_RECVMBOX_SIZE
#define DEFAULT_TCP_RECVMBOX_SIZE 32
/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /*__LWIPOPTS__H__ */
