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
#include "security_posture.h"
#include "system_time.h"
#include "system_watchdog.h"
#include "ota_manager.h"
#include "credential_store.h"
#include "device_config.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#ifndef NET_ENABLE_DNS_SELF_TEST
#define NET_ENABLE_DNS_SELF_TEST 0
#endif

#ifndef FIRMWARE_PRODUCTION_PROFILE
#define FIRMWARE_PRODUCTION_PROFILE 1U
#endif

#ifndef FLASH_STAGING_BUTTON_SELF_TEST_ENABLED
#define FLASH_STAGING_BUTTON_SELF_TEST_ENABLED \
    (FIRMWARE_PRODUCTION_PROFILE == 0U)
#endif

#ifndef RUNTIME_HEARTBEAT_LOGS_ENABLED
#define RUNTIME_HEARTBEAT_LOGS_ENABLED \
    (FIRMWARE_PRODUCTION_PROFILE == 0U)
#endif

#define FLASH_STAGING_BUTTON_HOLD_MS 1500U
#define FLASH_STAGING_BUTTON_SAMPLE_MS 20U
#define NET_DHCP_ACQUIRE_TIMEOUT_MS 30000U
#define NET_DHCP_RETRY_DELAY_MS      5000U
#define NET_TIME_SYNC_INTERVAL_MS    (6UL * 60UL * 60UL * 1000UL)
#define NET_TIME_SYNC_RETRY_MS       (15UL * 60UL * 1000UL)
#define OTA_TRIAL_MQTT_STABLE_MS     30000U
#define CREDENTIAL_ROTATION_MQTT_STABLE_MS 30000U
#define PWR_VOS_READY_TIMEOUT_MS       100U

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
  .stack_size = 16384,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
static osThreadId_t mqttServiceTaskHandle;
static const osThreadAttr_t mqttServiceTask_attributes = {
  .name = "mqttService",
  .stack_size = 65536,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
extern struct netif gnetif;
uint8_t tls_test_done = 0;
uint32_t last_blink_ms = 0;
uint32_t last_log_ms = 0;
static volatile uint8_t network_ready = 0U;
static volatile uint8_t tls_initialized = 0U;
static volatile uint8_t mqtt_ready = 0U;
static volatile uint8_t network_reconfigure_requested = 0U;
static volatile uint8_t network_force_dhcp_fallback = 0U;
static volatile uint8_t network_active_mode = DEVICE_NETWORK_DHCP;
static uint8_t network_cloud_connection_failures = 0U;
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
static void StartMqttServiceTask(void *argument);
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

void Net_RequestReconfigure(void)
{
    network_force_dhcp_fallback = 0U;
    network_cloud_connection_failures = 0U;
    network_reconfigure_requested = 1U;
}

const char *Net_GetActiveModeName(void)
{
    if ((network_active_mode == DEVICE_NETWORK_DHCP) &&
        (network_force_dhcp_fallback != 0U))
    {
        return "DHCP_FALLBACK";
    }
    return network_active_mode == DEVICE_NETWORK_STATIC ? "STATIC" : "DHCP";
}

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

  SecurityPosture_ConfigureMpu();

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
  mqttServiceTaskHandle = osThreadNew(StartMqttServiceTask,
                                      NULL,
                                      &mqttServiceTask_attributes);

  if ((defaultTaskHandle == NULL) || (mqttServiceTaskHandle == NULL))
  {
      SYS_LOG("RTOS: service task create FAILED");
      Error_Handler();
  }
  else
  {
      SYS_LOG("RTOS: service tasks create OK");
  }
  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* Returning from osKernelStart means the scheduler could not start. */
  Error_Handler();

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
  uint32_t vos_wait_started_ms;

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  vos_wait_started_ms = HAL_GetTick();
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
  {
    if ((HAL_GetTick() - vos_wait_started_ms) >= PWR_VOS_READY_TIMEOUT_MS)
    {
      NVIC_SystemReset();
    }
  }

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
        SystemWatchdog_Heartbeat(SYSTEM_WATCHDOG_SUPERVISOR);
        SystemWatchdog_Service();
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

static int Net_ConfigureConnectedInterface(void)
{
    DeviceConfig config;
    uint32_t start_ms;
    uint32_t last_log_ms;
    ip4_addr_t zero;
    err_t err;
    char buf[96];

    DeviceConfig_GetSnapshot(&config);

    if (!netif_is_link_up(&gnetif))
    {
        NET_LOG("NET: DHCP skipped, Ethernet link is down");
        return -1;
    }

    if ((config.network_addressing_mode == DEVICE_NETWORK_STATIC) &&
        (network_force_dhcp_fallback == 0U))
    {
        ip4_addr_t address;
        ip4_addr_t netmask;
        ip4_addr_t gateway;
        ip_addr_t dns_primary;
        ip_addr_t dns_secondary;

        IP4_ADDR(&address,
                 config.ipv4_address[0],
                 config.ipv4_address[1],
                 config.ipv4_address[2],
                 config.ipv4_address[3]);
        IP4_ADDR(&netmask,
                 config.ipv4_netmask[0],
                 config.ipv4_netmask[1],
                 config.ipv4_netmask[2],
                 config.ipv4_netmask[3]);
        IP4_ADDR(&gateway,
                 config.ipv4_gateway[0],
                 config.ipv4_gateway[1],
                 config.ipv4_gateway[2],
                 config.ipv4_gateway[3]);
        IP_ADDR4(&dns_primary,
                 config.dns_primary[0],
                 config.dns_primary[1],
                 config.dns_primary[2],
                 config.dns_primary[3]);
        IP_ADDR4(&dns_secondary,
                 config.dns_secondary[0],
                 config.dns_secondary[1],
                 config.dns_secondary[2],
                 config.dns_secondary[3]);

        NET_LOG("NET: static IPv4 configuration start");
        dhcp_stop(&gnetif);
        netif_set_addr(&gnetif, &address, &netmask, &gateway);
        netif_set_default(&gnetif);
        if (!netif_is_up(&gnetif))
        {
            netif_set_up(&gnetif);
        }
        dns_setserver(0U, &dns_primary);
        dns_setserver(1U, &dns_secondary);
        network_active_mode = DEVICE_NETWORK_STATIC;
        NET_LOG("NET: static IPv4 configuration applied");
        goto network_configuration_ready;
    }

    IP4_ADDR(&zero, 0, 0, 0, 0);
    NET_LOG(network_force_dhcp_fallback
                ? "NET: DHCP safety fallback start"
                : "NET: DHCP configuration start");
    dhcp_stop(&gnetif);
    netif_set_addr(&gnetif, &zero, &zero, &zero);
    netif_set_default(&gnetif);
    if (!netif_is_up(&gnetif))
    {
        netif_set_up(&gnetif);
    }

    osDelay(200);
    err = dhcp_start(&gnetif);
    if (err != ERR_OK)
    {
        NET_LOGF("NET: dhcp_start failed err=%d", (int)err);
        return -1;
    }

    start_ms = HAL_GetTick();
    last_log_ms = start_ms;
    NET_LOG("NET: waiting for DHCP lease...");

    while (!dhcp_supplied_address(&gnetif))
    {
        uint32_t now = HAL_GetTick();

        SystemWatchdog_Heartbeat(SYSTEM_WATCHDOG_SUPERVISOR);
        SystemWatchdog_Service();
        if (!netif_is_link_up(&gnetif))
        {
            NET_LOG("NET: link lost while waiting for DHCP");
            dhcp_stop(&gnetif);
            return -1;
        }

        if ((now - start_ms) >= NET_DHCP_ACQUIRE_TIMEOUT_MS)
        {
            NET_LOG("NET: DHCP lease timeout");
            dhcp_stop(&gnetif);
            return -1;
        }

        if ((now - last_log_ms) >= 5000U)
        {
            last_log_ms = now;
            NET_LOGF("NET: DHCP waiting elapsed=%lums",
                     (unsigned long)(now - start_ms));
        }

        osDelay(100);
    }

    NET_LOG("NET: DHCP lease acquired");
    network_active_mode = DEVICE_NETWORK_DHCP;

network_configuration_ready:
    Net_PrintIp();
    snprintf(buf, sizeof(buf), "NET: DNS0=%s", ipaddr_ntoa(dns_getserver(0)));
    NET_LOG(buf);
    snprintf(buf, sizeof(buf), "NET: DNS1=%s", ipaddr_ntoa(dns_getserver(1)));
    NET_LOG(buf);

    if (SystemTime_SyncNetwork() != 0)
    {
        if (!SystemTime_IsValid())
        {
            NET_LOG("TIME: no valid RTC/SNTP time; TLS is blocked");
            return -1;
        }
        NET_LOG("TIME: using validated RTC holdover");
    }
    NET_LOGF("TIME: state=%s",
             SystemTime_StateName(SystemTime_GetState()));

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

static void CredentialRotation_RecordConnectionFailure(const char *stage)
{
    int result;

    if (CredentialStore_GetState() != CREDENTIAL_STORE_ROTATION_VALIDATING)
    {
        return;
    }

    result = CredentialStore_RecordRotationFailure();
    if (result > 0)
    {
        SYS_LOGF("FLEET: rotated credential validation failed at %s, previous credentials restored",
                 stage);
        (void)IotEvents_QueueAlert(
            IOT_ALERT_WARNING,
            "CREDENTIAL_ROTATION_ROLLED_BACK",
            "New credentials failed validation; previous credentials restored");
    }
    else if (result == 0)
    {
        SYS_LOGF("FLEET: rotated credential validation retry stage=%s", stage);
    }
    else
    {
        SYS_LOG("FLEET: rotated credential validation state persistence failed");
        (void)IotEvents_QueueAlert(
            IOT_ALERT_CRITICAL,
            "CREDENTIAL_ROTATION_RECOVERY_FAILED",
            "Credential validation failure could not be persisted or rolled back");
    }
}

static void Network_RecordCloudConnectionFailure(void)
{
    DeviceConfig config;

    DeviceConfig_GetSnapshot(&config);
    if ((config.network_addressing_mode != DEVICE_NETWORK_STATIC) ||
        (config.network_dhcp_fallback_enabled == 0U) ||
        (network_force_dhcp_fallback != 0U))
    {
        return;
    }

    if (network_cloud_connection_failures < UINT8_MAX)
    {
        network_cloud_connection_failures++;
    }
    if (network_cloud_connection_failures >= 3U)
    {
        network_force_dhcp_fallback = 1U;
        network_reconfigure_requested = 1U;
        SYS_LOG("NET: static configuration failed cloud health gate; DHCP fallback scheduled");
        (void)IotEvents_QueueAlert(
            IOT_ALERT_WARNING,
            "STATIC_NETWORK_FALLBACK",
            "Static IPv4 could not reach AWS IoT; DHCP safety fallback scheduled");
    }
}

/*
 * This is the sole owner of all mbedTLS/MQTT contexts.  Bounded waits during
 * DNS, TCP, TLS, Fleet Provisioning and MQTT setup therefore cannot stop the
 * Ethernet link supervisor in defaultTask.
 */
static void StartMqttServiceTask(void *argument)
{
    uint32_t retry_after_ms = 0U;
    uint32_t mqtt_healthy_since_ms = 0U;
    uint32_t connectivity_outage_started_ms = 0U;
    uint8_t connectivity_outage_active = 0U;

    (void)argument;
    SYS_LOG("RTOS: mqttService start");

    for (;;)
    {
        uint32_t now = HAL_GetTick();
        SystemWatchdog_Heartbeat(SYSTEM_WATCHDOG_MQTT);
        OtaManager_Process(now);
        if (tls_initialized != 0U)
        {
            TlsClient_OfflineTelemetryProcess(now);
        }

        if (TlsClient_LifecycleResetIsDue())
        {
            SYS_LOG("LIFECYCLE: acknowledgement window completed; rebooting");
            TlsClient_Disconnect();
            osDelay(50U);
            __DSB();
            NVIC_SystemReset();
        }

        uint8_t usable_network =
            (tls_initialized != 0U) &&
            DeviceConfig_IsCloudAccessAllowed() &&
            (network_ready != 0U) &&
            netif_is_link_up(&gnetif) &&
            netif_is_up(&gnetif) &&
            (netif_ip4_addr(&gnetif)->addr != 0U);

        if (usable_network == 0U)
        {
            if (mqtt_ready != 0U)
            {
                if (connectivity_outage_active == 0U)
                {
                    connectivity_outage_active = 1U;
                    connectivity_outage_started_ms = now;
                    (void)IotEvents_QueueAlert(
                        IOT_ALERT_WARNING,
                        "NETWORK_CONNECTION_LOST",
                        "Network became unavailable; MQTT reconnect is deferred");
                }
                TlsClient_Disconnect();
                mqtt_ready = 0U;
                mqtt_healthy_since_ms = 0U;
            }
            osDelay(50U);
            continue;
        }

        if ((mqtt_ready == 0U) &&
            ((int32_t)(now - retry_after_ms) >= 0))
        {
            tls_test_done = 1U;
            retry_after_ms = now + 5000U;
            TlsClient_ClearCancel();
            SYS_LOG("TLS TEST: start");

            if (TlsClient_Connect() == 0)
            {
                SYS_LOG("TLS TEST: HANDSHAKE OK");
                SYS_LOG("MQTT TEST: start");

                if (TlsClient_MqttTest() == 0)
                {
                    if (connectivity_outage_active != 0U)
                    {
                        char recovery_message[128];
                        uint32_t outage_seconds =
                            (HAL_GetTick() - connectivity_outage_started_ms +
                             999U) / 1000U;

                        (void)snprintf(
                            recovery_message,
                            sizeof(recovery_message),
                            "MQTT connectivity restored after %lu second(s)",
                            (unsigned long)outage_seconds);
                        (void)IotEvents_QueueDiagnosticDurable(
                            IOT_DIAGNOSTIC_INFO,
                            "NETWORK",
                            "NETWORK_CONNECTION_RESTORED",
                            recovery_message);
                        connectivity_outage_active = 0U;
                    }
                    SYS_LOG("MQTT TEST: OK");
                    mqtt_ready = 1U;
                    mqtt_healthy_since_ms = HAL_GetTick();
                    network_cloud_connection_failures = 0U;
                }
                else
                {
                    SYS_LOG("MQTT TEST: FAIL");
                    CredentialRotation_RecordConnectionFailure("mqtt");
                    Network_RecordCloudConnectionFailure();
                    (void)IotEvents_QueueAlert(
                        IOT_ALERT_WARNING,
                        "MQTT_CONNECT_FAILED",
                        "MQTT session could not be established");
                    TlsClient_Disconnect();
                }
            }
            else
            {
                SYS_LOG("TLS TEST: HANDSHAKE FAIL");
                CredentialRotation_RecordConnectionFailure("tls");
                Network_RecordCloudConnectionFailure();
                (void)IotEvents_QueueAlert(
                    IOT_ALERT_WARNING,
                    "TLS_HANDSHAKE_FAILED",
                    "TLS handshake with AWS IoT failed");
            }
        }

        if (mqtt_ready != 0U)
        {
            if ((network_ready == 0U) ||
                !netif_is_link_up(&gnetif) ||
                !netif_is_up(&gnetif) ||
                (TlsClient_MqttProcess() != 0))
            {
                SYS_LOG("MQTT: session lost, reconnect scheduled");
                if (connectivity_outage_active == 0U)
                {
                    connectivity_outage_active = 1U;
                    connectivity_outage_started_ms = now;
                }
                (void)IotEvents_QueueAlert(
                    IOT_ALERT_WARNING,
                    "MQTT_SESSION_LOST",
                    "MQTT session was lost; reconnect scheduled");
                TlsClient_Disconnect();
                mqtt_ready = 0U;
                mqtt_healthy_since_ms = 0U;
                retry_after_ms = HAL_GetTick() + 5000U;
            }
        }

        if ((mqtt_ready != 0U) &&
            (mqtt_healthy_since_ms != 0U) &&
            (CredentialStore_GetState() ==
             CREDENTIAL_STORE_ROTATION_VALIDATING) &&
            ((HAL_GetTick() - mqtt_healthy_since_ms) >=
             CREDENTIAL_ROTATION_MQTT_STABLE_MS))
        {
            if (CredentialStore_CommitRotationValidated() == 0)
            {
                SYS_LOG("FLEET: rotated credentials validated and committed");
                (void)IotEvents_QueueDiagnostic(
                    IOT_DIAGNOSTIC_INFO,
                    "SECURITY",
                    "CREDENTIAL_ROTATION_SUCCEEDED",
                    "New credentials passed the stable MQTT health gate");
                if (TlsClient_PublishStatus() != 0)
                {
                    SYS_LOG("FLEET: committed credential status publish deferred");
                }
            }
            else
            {
                SYS_LOG("FLEET: rotated credential commit failed");
                mqtt_healthy_since_ms = HAL_GetTick();
            }
        }

        if ((mqtt_ready != 0U) &&
            (mqtt_healthy_since_ms != 0U) &&
            (OtaManager_IsTrial() != 0) &&
            (SystemWatchdog_IsStarted() != 0) &&
            ((HAL_GetTick() - mqtt_healthy_since_ms) >=
             OTA_TRIAL_MQTT_STABLE_MS))
        {
            if (OtaManager_ConfirmHealthy() == 0)
            {
                SYS_LOG("OTA: MQTT stability gate passed, image confirmed");
            }
            else
            {
                SYS_LOG("OTA: MQTT stability confirmation failed");
                mqtt_healthy_since_ms = HAL_GetTick();
            }
        }

        osDelay(10U);
    }
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
    NET_LOGF("NET: MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             gnetif.hwaddr[0],
             gnetif.hwaddr[1],
             gnetif.hwaddr[2],
             gnetif.hwaddr[3],
             gnetif.hwaddr[4],
             gnetif.hwaddr[5]);

    uint32_t network_retry_after_ms = 0U;
    uint32_t next_time_sync_ms = HAL_GetTick() + NET_TIME_SYNC_INTERVAL_MS;

    /* Local persistent services must be available even when Ethernet is down. */
    SYS_LOG("RTOS: local services init start");
    if (SystemTime_Init() == 0)
    {
        SYS_LOGF("TIME: RTC initialized state=%s",
                 SystemTime_StateName(SystemTime_GetState()));
    }
    else
    {
        SYS_LOG("TIME: RTC initialization FAILED");
    }

    if (TlsClient_Init() == 0)
    {
        tls_initialized = 1U;
        SYS_LOG("RTOS: local services init OK, TLS handshake available");
    }
    else
    {
        tls_initialized = 0U;
        SYS_LOG("RTOS: local services init FAILED");
    }

    SecurityPosture_Report();
    if (SystemWatchdog_InitAndStart() == 0)
    {
        SYS_LOG("WATCHDOG: IWDG started timeout~32s");
    }
    else
    {
        SYS_LOG("WATCHDOG: IWDG initialization FAILED");
    }

    /*
     * A TRIAL image is confirmed by mqttService only after the signed image,
     * persistent services, IWDG and a stable AWS MQTT session are all proven.
     */

    if (Net_WaitForLink() == 0)
    {
        network_ready = (Net_ConfigureConnectedInterface() == 0) ? 1U : 0U;
        if (!network_ready)
        {
            network_retry_after_ms = HAL_GetTick() + NET_DHCP_RETRY_DELAY_MS;
        }
    }
    else
    {
        NET_LOG("NET: no link, skipping DNS/TCP/TLS");
        network_ready = 0U;
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

        SystemWatchdog_Heartbeat(SYSTEM_WATCHDOG_SUPERVISOR);
        SystemWatchdog_Service();
        if (!mqtt_ready && ((now - last_blink_ms) >= 500U))
        {
            last_blink_ms = now;
            HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
        }

#if RUNTIME_HEARTBEAT_LOGS_ENABLED
        if ((now - last_log_ms) >= 5000U)
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
#endif

        if ((network_reconfigure_requested != 0U) &&
            netif_is_link_up(&gnetif))
        {
            network_reconfigure_requested = 0U;
            network_ready = 0U;
            TlsClient_RequestCancel();
            NET_LOG("NET: applying persisted network configuration");
            osDelay(250U);
            network_ready =
                (Net_ConfigureConnectedInterface() == 0) ? 1U : 0U;
            network_retry_after_ms = network_ready
                                         ? 0U
                                         : HAL_GetTick() +
                                               NET_DHCP_RETRY_DELAY_MS;
            if (network_ready != 0U)
            {
                (void)IotEvents_QueueDiagnosticDurable(
                    IOT_DIAGNOSTIC_INFO,
                    "NETWORK",
                    "NETWORK_CONFIGURATION_APPLIED",
                    Net_GetActiveModeName());
            }
        }

        if (!netif_is_link_up(&gnetif))
        {
            NET_LOG("NET: link DOWN, waiting again...");
            network_ready = 0U;
            TlsClient_RequestCancel();
            (void)IotEvents_QueueAlert(IOT_ALERT_WARNING,
                                       "ETHERNET_LINK_LOST",
                                       "Ethernet link was lost");

            if (Net_WaitForLink() == 0)
            {
                network_ready =
                    (Net_ConfigureConnectedInterface() == 0) ? 1U : 0U;

                if (network_ready)
                {
                    network_retry_after_ms = 0U;
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
                    network_retry_after_ms =
                        HAL_GetTick() + NET_DHCP_RETRY_DELAY_MS;
                    NET_LOG("NET: interface configuration failed after reconnect");
                }
            }
            else
            {
                network_ready = 0U;
            }
        }

        if (netif_is_link_up(&gnetif) &&
            (!network_ready || (netif_ip4_addr(&gnetif)->addr == 0U)) &&
            ((int32_t)(HAL_GetTick() - network_retry_after_ms) >= 0))
        {
            network_ready = 0U;
            TlsClient_RequestCancel();
            network_ready =
                (Net_ConfigureConnectedInterface() == 0) ? 1U : 0U;
            network_retry_after_ms = network_ready
                                         ? 0U
                                         : HAL_GetTick() + NET_DHCP_RETRY_DELAY_MS;
        }

        if (network_ready &&
            ((int32_t)(HAL_GetTick() - next_time_sync_ms) >= 0))
        {
            if (SystemTime_SyncNetwork() == 0)
            {
                next_time_sync_ms =
                    HAL_GetTick() + NET_TIME_SYNC_INTERVAL_MS;
                NET_LOG("TIME: periodic SNTP synchronization OK");
            }
            else
            {
                next_time_sync_ms = HAL_GetTick() + NET_TIME_SYNC_RETRY_MS;
                NET_LOG("TIME: periodic SNTP synchronization deferred");
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
