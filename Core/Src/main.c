/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "mbedtls.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lwip.h"
#include "net_debug_server.h"
#include "tls_client.h"
#include "debug_logger.h"
#include "iot_events.h"
#include "flash_staging.h"
#include "flash_staging_self_test.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include "lwip/etharp.h"
#include "lwip/api.h"
#include "aws_iot_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_it.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#ifndef NET_ENABLE_DNS_SELF_TEST
#define NET_ENABLE_DNS_SELF_TEST 0
#endif

#ifndef FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
#define FLASH_STAGING_BUTTON_SELF_TEST_ENABLED 1U
#endif

#define FLASH_STAGING_BUTTON_HOLD_MS 1500U
#define FLASH_STAGING_BUTTON_SAMPLE_MS 20U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

RNG_HandleTypeDef hrng;

UART_HandleTypeDef huart3;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 65536,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
extern struct netif gnetif;
uint8_t tls_test_done = 0;
uint32_t last_blink_ms = 0;
uint32_t last_log_ms = 0;
#if FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
static uint8_t flash_staging_self_test_requested = 0U;
#endif
extern volatile uint32_t eth_rx_irq_count;
extern volatile uint32_t eth_tx_irq_count;
extern volatile uint32_t eth_err_irq_count;
extern volatile uint32_t eth_input_pbuf_count;
extern volatile uint32_t eth_last_dma_error;
extern volatile uint32_t eth_last_hal_error;
#if NET_ENABLE_DNS_SELF_TEST
static volatile uint8_t net_dns_done = 0U;
static volatile err_t net_dns_err = ERR_INPROGRESS;
static volatile uint8_t net_dns_start_done = 0U;
static volatile err_t net_dns_start_err = ERR_INPROGRESS;
static ip_addr_t net_dns_addr;
static char net_dns_hostname[256];
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_RNG_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
static void Print_ResetReason(void);
#if FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
static uint8_t FlashStagingSelfTest_ButtonHeldAtBoot(void);
#endif
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define SYS_LOG(text)      Debug_LogLine(DEBUG_SRC_SYSTEM, (text))
#define SYS_LOGF(...)      Debug_Logf(DEBUG_SRC_SYSTEM, __VA_ARGS__)
#define NET_LOG(text)      Debug_LogLine(DEBUG_SRC_NET, (text))
#define NET_LOGF(...)      Debug_Logf(DEBUG_SRC_NET, __VA_ARGS__)

#if FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
static uint8_t FlashStagingSelfTest_ButtonHeldAtBoot(void)
{
    uint32_t start_ms;

    if (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) != GPIO_PIN_SET)
    {
        return 0U;
    }

    start_ms = HAL_GetTick();
    while ((HAL_GetTick() - start_ms) < FLASH_STAGING_BUTTON_HOLD_MS)
    {
        if (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) != GPIO_PIN_SET)
        {
            return 0U;
        }

        HAL_Delay(FLASH_STAGING_BUTTON_SAMPLE_MS);
    }

    return 1U;
}
#endif

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* Ethernet DMA descriptors, RX pool and the LwIP heap live in D2 SRAM. */
  __HAL_RCC_D2SRAM1_CLK_ENABLE();
  __HAL_RCC_D2SRAM2_CLK_ENABLE();
  __HAL_RCC_D2SRAM3_CLK_ENABLE();

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART3_UART_Init();
  MX_RNG_Init();
  /* Call PreOsInit function */
  MX_MBEDTLS_Init();
  /* USER CODE BEGIN 2 */
//  NetDebugServer_Init();
//  TlsClient_Init();
#if FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
  flash_staging_self_test_requested = FlashStagingSelfTest_ButtonHeldAtBoot();
#endif
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_DEBUG */
  DebugLogger_Init(&huart3);
  SYS_LOG("BOOT: program start");
  SYS_LOG("BOOT: GPIO init OK");
  SYS_LOG("BOOT: UART3 init OK");
  SYS_LOG("BOOT: before RTOS");
#if FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
  if (flash_staging_self_test_requested)
  {
      SYS_LOG("BOOT: Flash staging self-test requested by USER button");
  }
#endif
  Print_ResetReason();
  /* USER CODE END RTOS_DEBUG */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  if (defaultTaskHandle == NULL)
  {
      SYS_LOG("RTOS: defaultTask create FAILED");
  }
  else
  {
      SYS_LOG("RTOS: defaultTask create OK");
  }
  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      //to zakomentowalam to check that
      //    MX_LWIP_Process();
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

      uint32_t now = HAL_GetTick();

      if ((now - last_blink_ms) >= 500)
      {
          last_blink_ms = now;
          HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
      }

      if ((now - last_log_ms) >= 5000)
      {
          last_log_ms = now;
          SYS_LOG("LOOP: lwip alive");
      }

      NetDebugServer_Process();
  }

  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSI
                              |RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 19;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOMEDIUM;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief RNG Initialization Function
  * @param None
  * @retval None
  */
static void MX_RNG_Init(void)
{

  /* USER CODE BEGIN RNG_Init 0 */

  /* USER CODE END RNG_Init 0 */

  /* USER CODE BEGIN RNG_Init 1 */

  /* USER CODE END RNG_Init 1 */
  hrng.Instance = RNG;
  hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if (HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RNG_Init 2 */

  /* USER CODE END RNG_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LD1_Pin|LD3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(USB_OTG_FS_PWR_EN_GPIO_Port, USB_OTG_FS_PWR_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LD1_Pin LD3_Pin */
  GPIO_InitStruct.Pin = LD1_Pin|LD3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_OTG_FS_PWR_EN_Pin */
  GPIO_InitStruct.Pin = USB_OTG_FS_PWR_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_OTG_FS_PWR_EN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_OTG_FS_OVCR_Pin */
  GPIO_InitStruct.Pin = USB_OTG_FS_OVCR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USB_OTG_FS_OVCR_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PA8 PA11 PA12 */
  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_11|GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG1_FS;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void vAssertCalled(const char *file, int line)
{
    (void)file;
    (void)line;
    Fault_ReportFatal("FreeRTOS assert");
}

static void Print_ResetReason(void)
{
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) SYS_LOG("RESET: IWDG");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) SYS_LOG("RESET: WWDG");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   SYS_LOG("RESET: SOFTWARE");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))   SYS_LOG("RESET: POWER ON");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   SYS_LOG("RESET: NRST PIN");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   SYS_LOG("RESET: BROWNOUT");

    __HAL_RCC_CLEAR_RESET_FLAGS();
}

void vApplicationMallocFailedHook(void)
{
    Fault_ReportFatal("FreeRTOS malloc failed");
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    Fault_ReportFatal("FreeRTOS stack overflow");
}

static void Net_PrintIp(void)
{
    char ip[16];
    char mask[16];
    char gw[16];
    char buf[128];

    ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip, sizeof(ip));
    ip4addr_ntoa_r(netif_ip4_netmask(&gnetif), mask, sizeof(mask));
    ip4addr_ntoa_r(netif_ip4_gw(&gnetif), gw, sizeof(gw));

    snprintf(buf, sizeof(buf), "NET: IP=%s MASK=%s GW=%s\r\n", ip, mask, gw);
    NET_LOG(buf);
}

static int Net_WaitForLink(void)
{
    uint32_t start = HAL_GetTick();

    NET_LOG("NET: waiting for Ethernet link...");

    while (!netif_is_link_up(&gnetif))
    {
        if ((HAL_GetTick() - start) > 15000)
        {
            NET_LOG("NET: Ethernet link TIMEOUT");
            Net_PrintIp();
            return -1;
        }

        osDelay(100);
    }

    NET_LOG("NET: Ethernet link UP");

    NET_LOG("NET: before post-link delay");
    osDelay(1500);
    NET_LOG("NET: after post-link delay");

    NET_LOG("NET: before netif_is_up");

    if (!netif_is_up(&gnetif))
    {
        NET_LOG("NET: before netif_set_up");
        netif_set_up(&gnetif);
        NET_LOG("NET: netif set UP");
    }

    NET_LOG("NET: Net_WaitForLink return OK");

    return 0;
}

#if NET_ENABLE_DNS_SELF_TEST
static void Net_DnsFound(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)arg;

    if (ipaddr != NULL)
    {
        net_dns_addr = *ipaddr;
        net_dns_err = ERR_OK;
    }
    else
    {
        net_dns_err = ERR_VAL;
    }

    net_dns_done = 1U;
}

static void Net_DnsStartOnTcpipThread(void *arg)
{
    ip_addr_t addr;
    err_t err;

    (void)arg;

    err = dns_gethostbyname(net_dns_hostname, &addr, Net_DnsFound, NULL);
    net_dns_start_err = err;

    if (err == ERR_OK)
    {
        net_dns_addr = addr;
        net_dns_err = ERR_OK;
        net_dns_done = 1U;
    }
    else if (err != ERR_INPROGRESS)
    {
        net_dns_err = err;
        net_dns_done = 1U;
    }

    net_dns_start_done = 1U;
}

static int Net_TestDns(const char *hostname)
{
    char buf[200];
    char ipbuf[48];
    uint32_t t0;
    err_t err;

    NET_LOG("DNS: test start");

    snprintf(buf, sizeof(buf), "DNS: host=%s\r\n", hostname);
    NET_LOG(buf);

    snprintf(buf, sizeof(buf), "DNS: server0=%s\r\n", ipaddr_ntoa(dns_getserver(0)));
    NET_LOG(buf);

    net_dns_done = 0U;
    net_dns_err = ERR_INPROGRESS;
    net_dns_start_done = 0U;
    net_dns_start_err = ERR_INPROGRESS;
    memset(&net_dns_addr, 0, sizeof(net_dns_addr));
    snprintf(net_dns_hostname, sizeof(net_dns_hostname), "%s", hostname);
    net_dns_hostname[sizeof(net_dns_hostname) - 1U] = '\0';

    t0 = HAL_GetTick();

    NET_LOG("DNS: before tcpip_callback dns_gethostbyname");

    err = tcpip_callback(Net_DnsStartOnTcpipThread, NULL);

    snprintf(buf, sizeof(buf), "DNS: tcpip_callback err=%d\r\n", (int)err);
    NET_LOG(buf);

    if (err != ERR_OK)
    {
        return -1;
    }

    while (net_dns_start_done == 0U)
    {
        if ((HAL_GetTick() - t0) > 2000U)
        {
            NET_LOG("DNS: tcpip_callback start TIMEOUT");
            return -1;
        }

        osDelay(10);
    }

    snprintf(buf, sizeof(buf), "DNS: start err=%d\r\n", (int)net_dns_start_err);
    NET_LOG(buf);

    while (net_dns_done == 0U)
    {
        if ((HAL_GetTick() - t0) > 8000U)
        {
            NET_LOG("DNS: async resolve TIMEOUT");
            return -1;
        }

        osDelay(100);
    }

    if (net_dns_err != ERR_OK)
    {
        snprintf(buf, sizeof(buf), "DNS: async resolve failed err=%d\r\n", (int)net_dns_err);
        NET_LOG(buf);
        return -1;
    }

    ipaddr_ntoa_r(&net_dns_addr, ipbuf, sizeof(ipbuf));
    snprintf(buf, sizeof(buf),
             "DNS: resolved async -> %s dt=%lu ms\r\n",
             ipbuf,
             (unsigned long)(HAL_GetTick() - t0));
    NET_LOG(buf);
    return 0;
}
#endif

static int Net_TestTcpIp(const char *ip, uint16_t port)
{
    int sock;
    struct sockaddr_in addr;
    char buf[192];

    snprintf(buf, sizeof(buf), "TCPIP: connect %s:%u\r\n", ip, port);
    NET_LOG(buf);

    errno = 0;
    NET_LOG("TCPIP: before socket");

    sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    snprintf(buf, sizeof(buf), "TCPIP: after socket sock=%d errno=%d\r\n", sock, errno);
    NET_LOG(buf);

    if (sock < 0)
    {
        NET_LOG("TCPIP: socket failed");
        return -1;
    }

    int nonblock = 1;

    errno = 0;
    NET_LOG("TCPIP: before nonblock ioctl");

    int io_ret = lwip_ioctl(sock, FIONBIO, &nonblock);

    snprintf(buf, sizeof(buf), "TCPIP: after nonblock ioctl ret=%d errno=%d\r\n", io_ret, errno);
    NET_LOG(buf);

    if (io_ret < 0)
    {
        NET_LOG("TCPIP: nonblock ioctl failed");
        lwip_close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    errno = 0;
    NET_LOG("TCPIP: before connect");

    int ret = lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    snprintf(buf, sizeof(buf), "TCPIP: after connect ret=%d errno=%d\r\n", ret, errno);
    NET_LOG(buf);

    if (ret == 0)
    {
        NET_LOG("TCPIP: connect OK immediate");
        lwip_close(sock);
        return 0;
    }

    /*
     * Nie uzywamy blokujacego oczekiwania na socket, bo u Ciebie potrafi wisiec.
     * Pollujemy SO_ERROR recznie przez max 30 sekund.
     */
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < 30000)
    {
        int err = 0;
        socklen_t len = sizeof(err);

        errno = 0;
        int gs_ret = lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);

        snprintf(buf, sizeof(buf),
                 "TCPIP: poll getsockopt ret=%d sockerr=%d errno=%d t=%lu\r\n",
                 gs_ret,
                 err,
                 errno,
                 (unsigned long)(HAL_GetTick() - start));
        NET_LOG(buf);

        if (gs_ret == 0 && err == 0)
        {
            uint32_t elapsed = HAL_GetTick() - start;

            if (elapsed >= 1000)
            {
                NET_LOG("TCPIP: connect probably OK after delay by SO_ERROR=0");
                lwip_close(sock);
                return 0;
            }
        }

        if (err != 0 && err != EINPROGRESS && err != EALREADY && err != EWOULDBLOCK && err != EAGAIN)
        {
            snprintf(buf, sizeof(buf), "TCPIP: connect failed sockerr=%d\r\n", err);
            NET_LOG(buf);
            lwip_close(sock);
            return -1;
        }

        osDelay(1000);
    }

    NET_LOG("TCPIP: connect TIMEOUT manual poll");
    lwip_close(sock);
    return -1;
}

static void Net_SetStaticIp(void)
{
    NET_LOG("STATIC: enter");

    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    IP4_ADDR(&ipaddr, 192, 168, 18, 222);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 18, 1);

    dhcp_stop(&gnetif);

    netif_set_addr(&gnetif, &ipaddr, &netmask, &gw);
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);

    ip_addr_t dns1;
    ip_addr_t dns2;
    IP4_ADDR(&dns1, 192, 168, 18, 1);
    IP4_ADDR(&dns2, 8, 8, 8, 8);

    dns_setserver(0, &dns1);
    dns_setserver(1, &dns2);

    NET_LOG("NET: static IP set");

    char buf[96];

    snprintf(buf, sizeof(buf), "NET: DNS0=%s\r\n", ipaddr_ntoa(dns_getserver(0)));
    NET_LOG(buf);

    snprintf(buf, sizeof(buf), "NET: DNS1=%s\r\n", ipaddr_ntoa(dns_getserver(1)));
    NET_LOG(buf);

    Net_PrintIp();

    NET_LOG("NET: static config done");
}

static int Net_ConfigureConnectedInterface(void)
{
    NET_LOG("STEP: before Net_SetStaticIp");
    Net_SetStaticIp();
    NET_LOG("STEP: after Net_SetStaticIp");

#if NET_ENABLE_DNS_SELF_TEST
    NET_LOG("NET: ready for DNS-only test");
    NET_LOG("STEP: DNS example.com");

    if (Net_TestDns("example.com") != 0)
    {
        NET_LOG("DNS TEST: example.com FAILED");
        return -1;
    }

    NET_LOG("DNS TEST: example.com OK");
    NET_LOG("STEP: DNS AWS");

    if (Net_TestDns(AWS_IOT_ENDPOINT) != 0)
    {
        NET_LOG("DNS TEST: AWS FAILED");
        return -1;
    }

    NET_LOG("DNS TEST: AWS OK");
#else
    NET_LOG("NET: DNS self-test disabled");
    NET_LOG("NET: TLS task will resolve AWS host");
#endif

    return 0;
}

static void FlashStaging_LogSnapshot(const char *source)
{
    const FlashStagingInfo *info = FlashStaging_GetInfo();

    SYS_LOGF("FLASH STAGING: state=%s capacity=%lu size=%lu source=%s",
             FlashStaging_StateName(info->state),
             (unsigned long)info->capacity_bytes,
             (unsigned long)info->file_size_bytes,
             source);
}

static void Net_RestartDhcp(void)
{
    char buf[80];
    err_t err;
    ip4_addr_t zero;

    IP4_ADDR(&zero, 0, 0, 0, 0);

    NET_LOG("NET: DHCP restart");

    dhcp_stop(&gnetif);
    netif_set_addr(&gnetif, &zero, &zero, &zero);

    osDelay(500);

    if (!netif_is_up(&gnetif))
    {
        netif_set_up(&gnetif);
    }

    osDelay(200);

    err = dhcp_start(&gnetif);

    snprintf(buf, sizeof(buf), "NET: dhcp_start err=%d\r\n", (int)err);
    NET_LOG(buf);
}

static void Net_WaitForLinkAndDhcp(void)
{
    uint32_t last_log;
    uint32_t last_retry;

    NET_LOG("NET: waiting for Ethernet link...");

    while (!netif_is_link_up(&gnetif))
    {
        osDelay(100);
    }

    NET_LOG("NET: Ethernet link UP");

    osDelay(1500);

    if (!netif_is_up(&gnetif))
    {
        netif_set_up(&gnetif);
        NET_LOG("NET: netif set UP");
    }

    Net_RestartDhcp();

    last_log = HAL_GetTick();
    last_retry = HAL_GetTick();

    NET_LOG("NET: waiting for DHCP...");

    while (!dhcp_supplied_address(&gnetif))
    {
        uint32_t now = HAL_GetTick();

        if ((now - last_log) >= 2000)
        {
            char ip[16];
            char buf[128];

            last_log = now;

            ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip, sizeof(ip));

            snprintf(buf, sizeof(buf),
                     "NET: link=%d netif=%d dhcp=%d ip=%s\r\n",
                     netif_is_link_up(&gnetif),
                     netif_is_up(&gnetif),
                     dhcp_supplied_address(&gnetif),
                     ip);

            NET_LOG(buf);
        }

        if ((now - last_retry) >= 10000)
        {
            last_retry = now;
            Net_RestartDhcp();
        }

        osDelay(100);
    }

    NET_LOG("NET: DHCP OK");
    Net_PrintIp();
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
    SYS_LOG("RTOS: defaultTask start");

#if FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
    if (flash_staging_self_test_requested)
    {
        FlashStagingResult staging_init_result = FlashStaging_Init();
        FlashStagingResult staging_test_result;

        SYS_LOGF("FLASH SELFTEST: init result=%d state=%s",
                 (int)staging_init_result,
                 FlashStaging_StateName(FlashStaging_GetInfo()->state));
        staging_test_result = FlashStagingSelfTest_Run();
        SYS_LOGF("FLASH SELFTEST: complete result=%d state=%s",
                 (int)staging_test_result,
                 FlashStaging_StateName(FlashStaging_GetInfo()->state));
        flash_staging_self_test_requested = 0U;
    }
#endif

    HAL_NVIC_SetPriority(ETH_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);

    MX_LWIP_Init();

    SYS_LOG("RTOS: LWIP init OK");

    int network_ready = 0;
    int tls_initialized = 0;
    int mqtt_ready = 0;
    uint32_t mqtt_retry_after_ms = 0U;

    /* Local persistent services must be available even when Ethernet is down. */
    SYS_LOG("RTOS: local services init start");
    if (TlsClient_Init() == 0)
    {
        tls_initialized = 1;
        SYS_LOG("RTOS: local services init OK, TLS handshake available");
    }
    else
    {
        tls_initialized = 0;
        SYS_LOG("RTOS: local services init FAILED");
    }

    if (Net_WaitForLink() == 0)
    {
        network_ready = (Net_ConfigureConnectedInterface() == 0);
    }
    else
    {
        NET_LOG("NET: no link, skipping DNS/TCP/TLS");
        network_ready = 0;
    }

    if (network_ready)
    {
        SYS_LOG("RTOS: network ready, TLS handshake enabled");
    }
    else
    {
        SYS_LOG("RTOS: network unavailable, TLS/MQTT deferred");
    }

    SYS_LOG("RTOS: network services init OK");

    for (;;)
    {
        uint32_t now = HAL_GetTick();

        if (!mqtt_ready && ((now - last_blink_ms) >= 500U))
        {
            last_blink_ms = now;
            HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
        }

        if ((now - last_log_ms) >= 5000)
        {
            last_log_ms = now;
            char buf[160];

            snprintf(buf, sizeof(buf),
                     "TASK: alive link=%d netif=%d ip=0x%08lx rx_irq=%lu tx_irq=%lu err=%lu pbuf=%lu dma=0x%08lx hal=0x%08lx\r\n",
                     netif_is_link_up(&gnetif),
                     netif_is_up(&gnetif),
                     (unsigned long)netif_ip4_addr(&gnetif)->addr,
                     eth_rx_irq_count,
                     eth_tx_irq_count,
                     eth_err_irq_count,
                     eth_input_pbuf_count,
                     eth_last_dma_error,
                     eth_last_hal_error);

            NET_LOG(buf);
        }

        if (!netif_is_link_up(&gnetif))
        {
            NET_LOG("NET: link DOWN, waiting again...");
            (void)IotEvents_QueueAlert(IOT_ALERT_WARNING,
                                       "ETHERNET_LINK_LOST",
                                       "Ethernet link was lost");

            if (Net_WaitForLink() == 0)
            {
                network_ready = (Net_ConfigureConnectedInterface() == 0);

                if (network_ready)
                {
                    FlashStaging_LogSnapshot("ethernet-restored");
                    osDelay(1000);
                    NET_LOG("NET: ready after reconnect");
                    (void)IotEvents_QueueDiagnostic(IOT_DIAGNOSTIC_INFO,
                                                    "ETHERNET",
                                                    "ETHERNET_LINK_RESTORED",
                                                    "Ethernet link was restored");
                }
                else
                {
                    NET_LOG("NET: interface configuration failed after reconnect");
                }
            }
            else
            {
                network_ready = 0;
            }
        }

        if (!mqtt_ready &&
            ((int32_t)(now - mqtt_retry_after_ms) >= 0) &&
            network_ready &&
            tls_initialized &&
            netif_is_link_up(&gnetif) &&
            netif_is_up(&gnetif) &&
            netif_ip4_addr(&gnetif)->addr != 0)
        {
            tls_test_done = 1;
            mqtt_retry_after_ms = now + 5000U;

            SYS_LOG("TLS TEST: start");

            if (TlsClient_Connect() == 0)
            {
                SYS_LOG("TLS TEST: HANDSHAKE OK");

                SYS_LOG("MQTT TEST: start");

                if (TlsClient_MqttTest() == 0)
                {
                    SYS_LOG("MQTT TEST: OK");
                    mqtt_ready = 1;
                }
                else
                {
                    SYS_LOG("MQTT TEST: FAIL");
                    (void)IotEvents_QueueAlert(IOT_ALERT_WARNING,
                                               "MQTT_CONNECT_FAILED",
                                               "MQTT session could not be established");
                    TlsClient_Disconnect();
                }
            }
            else
            {
                SYS_LOG("TLS TEST: HANDSHAKE FAIL");
                (void)IotEvents_QueueAlert(IOT_ALERT_WARNING,
                                           "TLS_HANDSHAKE_FAILED",
                                           "TLS handshake with AWS IoT failed");
            }
        }

        if (mqtt_ready)
        {
            if (!netif_is_link_up(&gnetif) ||
                !netif_is_up(&gnetif) ||
                (TlsClient_MqttProcess() != 0))
            {
                SYS_LOG("MQTT: session lost, reconnect scheduled");
                (void)IotEvents_QueueAlert(IOT_ALERT_WARNING,
                                           "MQTT_SESSION_LOST",
                                           "MQTT session was lost; reconnect scheduled");
                TlsClient_Disconnect();
                mqtt_ready = 0;
                mqtt_retry_after_ms = HAL_GetTick() + 5000U;
            }
        }

        osDelay(10);
    }
  /* USER CODE END 5 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number. */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
