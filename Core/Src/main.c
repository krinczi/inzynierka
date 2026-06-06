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
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include "lwip/etharp.h"
#include "lwip/api.h"
#include "aws_iot_config.h"
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

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
extern volatile uint32_t eth_rx_irq_count;
extern volatile uint32_t eth_tx_irq_count;
extern volatile uint32_t eth_err_irq_count;
extern volatile uint32_t eth_input_pbuf_count;
extern volatile uint32_t eth_last_dma_error;
extern volatile uint32_t eth_last_hal_error;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_RNG_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
static void UartPuts(const char *s);
static void Print_ResetReason(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART3_UART_Init();
  Print_ResetReason();
  MX_RNG_Init();
  /* Call PreOsInit function */
  MX_MBEDTLS_Init();
  /* USER CODE BEGIN 2 */
  HAL_UART_Transmit(&huart3, (uint8_t *)"BOOT: program start\r\n", 21, HAL_MAX_DELAY);
  HAL_UART_Transmit(&huart3, (uint8_t *)"BOOT: GPIO init OK\r\n", 20, HAL_MAX_DELAY);
  HAL_UART_Transmit(&huart3, (uint8_t *)"BOOT: UART3 init OK\r\n", 21, HAL_MAX_DELAY);
  HAL_UART_Transmit(&huart3, (uint8_t *)"BOOT: before RTOS\r\n", 19, HAL_MAX_DELAY);

//  NetDebugServer_Init();
//  TlsClient_Init();
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

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
      HAL_UART_Transmit(&huart3,
                        (uint8_t *)"RTOS: defaultTask create FAILED\r\n",
                        33,
                        HAL_MAX_DELAY);

      while (1)
      {
          HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
          HAL_Delay(200);
      }
  }
  else
  {
      HAL_UART_Transmit(&huart3,
                        (uint8_t *)"RTOS: defaultTask create OK\r\n",
                        29,
                        HAL_MAX_DELAY);
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
	  //to zakomentowałam to check that
	  //	  MX_LWIP_Process();
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
	      HAL_UART_Transmit(&huart3, (uint8_t *)"LOOP: lwip alive\r\n", 18, HAL_MAX_DELAY);	  }

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
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

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
    char buf[220];

    snprintf(buf, sizeof(buf),
             "\r\nFREERTOS ASSERT: %s:%d\r\n",
             file ? file : "unknown",
             line);

    UartPuts(buf);

    taskDISABLE_INTERRUPTS();

    while (1)
    {
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);

        for (volatile uint32_t i = 0; i < 1600000; i++)
        {
            __NOP();
        }
    }
}
static void Print_ResetReason(void)
{
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) UartPuts("RESET: IWDG\r\n");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) UartPuts("RESET: WWDG\r\n");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   UartPuts("RESET: SOFTWARE\r\n");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))   UartPuts("RESET: POWER ON\r\n");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   UartPuts("RESET: NRST PIN\r\n");
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   UartPuts("RESET: BROWNOUT\r\n");

    __HAL_RCC_CLEAR_RESET_FLAGS();
}
static void UartPuts(const char *s)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)s, strlen(s), HAL_MAX_DELAY);
}
void vApplicationMallocFailedHook(void)
{
    UartPuts("\r\nFREERTOS: MALLOC FAILED\r\n");

    taskDISABLE_INTERRUPTS();

    while (1)
    {
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);

        for (volatile uint32_t i = 0; i < 800000; i++)
        {
            __NOP();
        }
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;

    UartPuts("\r\nFREERTOS: STACK OVERFLOW in task: ");
    UartPuts(pcTaskName ? pcTaskName : "unknown");
    UartPuts("\r\n");

    taskDISABLE_INTERRUPTS();

    while (1)
    {
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);

        for (volatile uint32_t i = 0; i < 1600000; i++)
        {
            __NOP();
        }
    }
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
    UartPuts(buf);
}
static int Net_WaitForLink(void)
{
    uint32_t start = HAL_GetTick();

    UartPuts("NET: waiting for Ethernet link...\r\n");

    while (!netif_is_link_up(&gnetif))
    {
        if ((HAL_GetTick() - start) > 15000)
        {
            UartPuts("NET: Ethernet link TIMEOUT\r\n");
            Net_PrintIp();
            return -1;
        }

        osDelay(100);
    }

    UartPuts("NET: Ethernet link UP\r\n");

    UartPuts("NET: before post-link delay\r\n");
    osDelay(1500);
    UartPuts("NET: after post-link delay\r\n");

    UartPuts("NET: before netif_is_up\r\n");

    if (!netif_is_up(&gnetif))
    {
        UartPuts("NET: before netif_set_up\r\n");
        netif_set_up(&gnetif);
        UartPuts("NET: netif set UP\r\n");
    }

    UartPuts("NET: Net_WaitForLink return OK\r\n");

    return 0;
}

static int Net_TestDns(const char *hostname)
{
    ip_addr_t addr;
    char buf[200];
    char ipbuf[48];

    UartPuts("DNS: test start\r\n");

    snprintf(buf, sizeof(buf), "DNS: host=%s\r\n", hostname);
    UartPuts(buf);

    snprintf(buf, sizeof(buf), "DNS: server0=%s\r\n", ipaddr_ntoa(dns_getserver(0)));
    UartPuts(buf);

    uint32_t t0 = HAL_GetTick();

    UartPuts("DNS: before netconn_gethostbyname\r\n");

    err_t err = netconn_gethostbyname(hostname, &addr);

    uint32_t dt = HAL_GetTick() - t0;

    snprintf(buf, sizeof(buf),
             "DNS: after netconn_gethostbyname err=%d dt=%lu ms\r\n",
             (int)err,
             (unsigned long)dt);
    UartPuts(buf);

    if (err == ERR_OK)
    {
        ipaddr_ntoa_r(&addr, ipbuf, sizeof(ipbuf));
        snprintf(buf, sizeof(buf), "DNS: resolved -> %s\r\n", ipbuf);
        UartPuts(buf);
        return 0;
    }

    UartPuts("DNS: resolve failed\r\n");
    return -1;
}
static int Net_TestTcpIp(const char *ip, uint16_t port)
{
    int sock;
    struct sockaddr_in addr;
    char buf[192];

    snprintf(buf, sizeof(buf), "TCPIP: connect %s:%u\r\n", ip, port);
    UartPuts(buf);

    errno = 0;
    UartPuts("TCPIP: before socket\r\n");

    sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    snprintf(buf, sizeof(buf), "TCPIP: after socket sock=%d errno=%d\r\n", sock, errno);
    UartPuts(buf);

    if (sock < 0)
    {
        UartPuts("TCPIP: socket failed\r\n");
        return -1;
    }

    int nonblock = 1;

    errno = 0;
    UartPuts("TCPIP: before nonblock ioctl\r\n");

    int io_ret = lwip_ioctl(sock, FIONBIO, &nonblock);

    snprintf(buf, sizeof(buf), "TCPIP: after nonblock ioctl ret=%d errno=%d\r\n", io_ret, errno);
    UartPuts(buf);

    if (io_ret < 0)
    {
        UartPuts("TCPIP: nonblock ioctl failed\r\n");
        lwip_close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    errno = 0;
    UartPuts("TCPIP: before connect\r\n");

    int ret = lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    snprintf(buf, sizeof(buf), "TCPIP: after connect ret=%d errno=%d\r\n", ret, errno);
    UartPuts(buf);

    if (ret == 0)
    {
        UartPuts("TCPIP: connect OK immediate\r\n");
        lwip_close(sock);
        return 0;
    }

    /*
     * NIE używamy lwip_select(), bo u Ciebie potrafi wisieć.
     * Pollujemy SO_ERROR ręcznie przez max 30 sekund.
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
        UartPuts(buf);

        if (gs_ret == 0 && err == 0)
        {
            uint32_t elapsed = HAL_GetTick() - start;

            if (elapsed >= 1000)
            {
                UartPuts("TCPIP: connect probably OK after delay by SO_ERROR=0\r\n");
                lwip_close(sock);
                return 0;
            }
        }
        if (err != 0 && err != EINPROGRESS && err != EALREADY && err != EWOULDBLOCK && err != EAGAIN)
        {
            snprintf(buf, sizeof(buf), "TCPIP: connect failed sockerr=%d\r\n", err);
            UartPuts(buf);
            lwip_close(sock);
            return -1;
        }

        osDelay(1000);
    }

    UartPuts("TCPIP: connect TIMEOUT manual poll\r\n");
    lwip_close(sock);
    return -1;
}
static void Net_SetStaticIp(void)
{
    UartPuts("STATIC: enter\r\n");

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


    UartPuts("NET: static IP set\r\n");

    char buf[96];

    snprintf(buf, sizeof(buf), "NET: DNS0=%s\r\n", ipaddr_ntoa(dns_getserver(0)));
    UartPuts(buf);

    snprintf(buf, sizeof(buf), "NET: DNS1=%s\r\n", ipaddr_ntoa(dns_getserver(1)));
    UartPuts(buf);

    Net_PrintIp();

    UartPuts("NET: static config done\r\n");
}
static void Net_RestartDhcp(void)
{
    char buf[80];
    err_t err;
    ip4_addr_t zero;

    IP4_ADDR(&zero, 0, 0, 0, 0);

    UartPuts("NET: DHCP restart\r\n");

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
    UartPuts(buf);
}

static void Net_WaitForLinkAndDhcp(void)
{
    uint32_t last_log;
    uint32_t last_retry;

    UartPuts("NET: waiting for Ethernet link...\r\n");

    while (!netif_is_link_up(&gnetif))
    {
        osDelay(100);
    }

    UartPuts("NET: Ethernet link UP\r\n");

    osDelay(1500);

    if (!netif_is_up(&gnetif))
    {
        netif_set_up(&gnetif);
        UartPuts("NET: netif set UP\r\n");
    }

    Net_RestartDhcp();

    last_log = HAL_GetTick();
    last_retry = HAL_GetTick();

    UartPuts("NET: waiting for DHCP...\r\n");

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

            UartPuts(buf);
        }

        if ((now - last_retry) >= 10000)
        {
            last_retry = now;
            Net_RestartDhcp();
        }

        osDelay(100);
    }

    UartPuts("NET: DHCP OK\r\n");
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
    UartPuts("RTOS: defaultTask start\r\n");

    HAL_NVIC_SetPriority(ETH_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);

    MX_LWIP_Init();

    UartPuts("RTOS: LWIP init OK\r\n");

    int network_ready = 0;
    int tls_initialized = 0;

    if (Net_WaitForLink() == 0)
    {
        UartPuts("STEP: before Net_SetStaticIp\r\n");
        Net_SetStaticIp();
        UartPuts("STEP: after Net_SetStaticIp\r\n");

        osDelay(3000);

        {
            char buf[180];
            snprintf(buf, sizeof(buf),
                     "NET: pre-DNS link=%d netif=%d ip=0x%08lx rx_irq=%lu tx_irq=%lu err=%lu pbuf=%lu dma=0x%08lx hal=0x%08lx\r\n",
                     netif_is_link_up(&gnetif),
                     netif_is_up(&gnetif),
                     (unsigned long)netif_ip4_addr(&gnetif)->addr,
                     eth_rx_irq_count,
                     eth_tx_irq_count,
                     eth_err_irq_count,
                     eth_input_pbuf_count,
                     eth_last_dma_error,
                     eth_last_hal_error);
            UartPuts(buf);
        }

        UartPuts("NET: ready for DNS-only test\r\n");

        UartPuts("STEP: DNS example.com\r\n");
        int dns_example_ok = Net_TestDns("example.com");

        if (dns_example_ok == 0)
        {
            UartPuts("DNS TEST: example.com OK\r\n");

            UartPuts("STEP: DNS AWS\r\n");
            int dns_aws_ok = Net_TestDns(AWS_IOT_ENDPOINT);

            if (dns_aws_ok == 0)
            {
                UartPuts("DNS TEST: AWS OK\r\n");
                network_ready = 1;
            }
            else
            {
                UartPuts("DNS TEST: AWS FAILED\r\n");
                network_ready = 0;
            }
        }
        else
        {
            UartPuts("DNS TEST: example.com FAILED\r\n");
            network_ready = 0;
        }
    }
    else
    {
        UartPuts("NET: no link, skipping DNS/TCP/TLS\r\n");
        network_ready = 0;
    }

    /*
     * Na czas debugowania NIE odpalamy TCP ani TLS.
     * Najpierw DNS musi działać stabilnie.
     */
    tls_initialized = 0;

    if (network_ready)
    {
        UartPuts("RTOS: DNS OK, but TLS disabled for debug\r\n");
    }
    else
    {
        UartPuts("RTOS: DNS failed, TLS skipped\r\n");
    }

    UartPuts("RTOS: network services init OK\r\n");
    for (;;)
    {
        uint32_t now = HAL_GetTick();

        if ((now - last_blink_ms) >= 500)
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

            UartPuts(buf);
        }

        if (!netif_is_link_up(&gnetif))
        {
            UartPuts("NET: link DOWN, waiting again...\r\n");

            if (Net_WaitForLink() == 0)
            {
                Net_SetStaticIp();
                osDelay(1000);
                UartPuts("NET: ready for ping test after reconnect\r\n");
            }
        }
//        if (!tls_test_done &&
//            network_ready &&
//            tls_initialized &&
//            netif_is_link_up(&gnetif) &&
//            netif_is_up(&gnetif) &&
//            netif_ip4_addr(&gnetif)->addr != 0)
//        {
//            tls_test_done = 1;
//
//            HAL_UART_Transmit(&huart3, (uint8_t *)"TLS TEST: start\r\n", 17, HAL_MAX_DELAY);
//
//            if (TlsClient_Connect() == 0)
//            {
//                HAL_UART_Transmit(&huart3, (uint8_t *)"TLS TEST: HANDSHAKE OK\r\n", 25, HAL_MAX_DELAY);
//                TlsClient_Disconnect();
//            }
//            else
//            {
//                HAL_UART_Transmit(&huart3, (uint8_t *)"TLS TEST: HANDSHAKE FAIL\r\n", 27, HAL_MAX_DELAY);
//            }
//        }
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
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
