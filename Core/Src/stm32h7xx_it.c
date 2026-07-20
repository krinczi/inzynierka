/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "stm32h7xx_it.h"
#include "FreeRTOS.h"
#include "task.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void Fault_UartWriteChar(char value)
{
  uint32_t guard = 1000000U;

  while ((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U)
  {
    if (--guard == 0U)
    {
      return;
    }
  }

  USART3->TDR = (uint8_t)value;
}

static void Fault_UartWrite(const char *text)
{
  while (*text != '\0')
  {
    Fault_UartWriteChar(*text++);
  }
}

static void Fault_UartWriteHex(uint32_t value)
{
  static const char digits[] = "0123456789ABCDEF";
  int32_t shift;

  for (shift = 28; shift >= 0; shift -= 4)
  {
    Fault_UartWriteChar(digits[(value >> (uint32_t)shift) & 0x0FU]);
  }
}

static uint8_t Fault_StackFrameIsReadable(const uint32_t *stack_frame)
{
  uintptr_t address = (uintptr_t)stack_frame;

  return ((address >= 0x20000000UL) &&
          (address <= (0x20020000UL - (8UL * sizeof(uint32_t))))) ||
         ((address >= 0x24000000UL) &&
          (address <= (0x24080000UL - (8UL * sizeof(uint32_t))))) ||
         ((address >= 0x30000000UL) &&
          (address <= (0x30048000UL - (8UL * sizeof(uint32_t)))));
}

static void Fault_Report(const char *name,
                         const uint32_t *stack_frame,
                         uint32_t exc_return)
{
  __disable_irq();

  /* Bez HAL i bez RTOS: ten zapis dziala takze, gdy logger lub scheduler stoi. */
  Fault_UartWrite("\r\n[FATAL] ");
  Fault_UartWrite(name);
  Fault_UartWrite(" CFSR=0x");
  Fault_UartWriteHex(SCB->CFSR);
  Fault_UartWrite(" HFSR=0x");
  Fault_UartWriteHex(SCB->HFSR);
  Fault_UartWrite(" ABFSR=0x");
  Fault_UartWriteHex(SCB->ABFSR);
  Fault_UartWrite(" MMFAR=0x");
  Fault_UartWriteHex(SCB->MMFAR);
  Fault_UartWrite(" BFAR=0x");
  Fault_UartWriteHex(SCB->BFAR);
  Fault_UartWrite("\r\n");

  Fault_UartWrite("[FATAL] EXC_RETURN=0x");
  Fault_UartWriteHex(exc_return);
  Fault_UartWrite(" SP=0x");
  Fault_UartWriteHex((uint32_t)(uintptr_t)stack_frame);
  Fault_UartWrite("\r\n");

  if (Fault_StackFrameIsReadable(stack_frame) != 0U)
  {
    Fault_UartWrite("[FATAL] R0=0x");
    Fault_UartWriteHex(stack_frame[0]);
    Fault_UartWrite(" R1=0x");
    Fault_UartWriteHex(stack_frame[1]);
    Fault_UartWrite(" R2=0x");
    Fault_UartWriteHex(stack_frame[2]);
    Fault_UartWrite(" R3=0x");
    Fault_UartWriteHex(stack_frame[3]);
    Fault_UartWrite("\r\n");
    Fault_UartWrite("[FATAL] R12=0x");
    Fault_UartWriteHex(stack_frame[4]);
    Fault_UartWrite(" LR=0x");
    Fault_UartWriteHex(stack_frame[5]);
    Fault_UartWrite(" PC=0x");
    Fault_UartWriteHex(stack_frame[6]);
    Fault_UartWrite(" XPSR=0x");
    Fault_UartWriteHex(stack_frame[7]);
    Fault_UartWrite("\r\n");
  }
  else
  {
    Fault_UartWrite("[FATAL] stack frame outside known SRAM\r\n");
  }

  while (1)
  {
  }
}

void Fault_ReportFatal(const char *name)
{
  Fault_Report(name, NULL, 0U);
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */
extern ETH_HandleTypeDef heth;
extern UART_HandleTypeDef huart3;
/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  uint32_t exc_return;
  const uint32_t *stack_frame;

  __asm volatile ("mov %0, lr" : "=r" (exc_return));
  stack_frame = ((exc_return & 4U) != 0U)
                  ? (const uint32_t *)__get_PSP()
                  : (const uint32_t *)__get_MSP();
  Fault_Report("HardFault", stack_frame, exc_return);
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  Fault_Report("MemManage", NULL, 0U);
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  Fault_Report("BusFault", NULL, 0U);
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  Fault_Report("UsageFault", NULL, 0U);
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
#if (INCLUDE_xTaskGetSchedulerState == 1 )
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
#endif /* INCLUDE_xTaskGetSchedulerState */
  xPortSysTickHandler();
#if (INCLUDE_xTaskGetSchedulerState == 1 )
  }
#endif /* INCLUDE_xTaskGetSchedulerState */
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */
void ETH_IRQHandler(void)
{
  HAL_ETH_IRQHandler(&heth);
}
void USART3_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart3);
}
/* USER CODE END 1 */
