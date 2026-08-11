/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    hardware_rng.c
  * @author  MCD Application Team
  * @version V1.2.1
  * @date    14-April-2017
  * @brief   mbedtls alternate entropy data function.
  *          the mbedtls_hardware_poll() is customized to use the STM32 RNG
  *          to generate random data, required for TLS encryption algorithms.
  *
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

#include "mbedtls/build_info.h"
#include "mbedtls_platform_rng.h"

#ifdef MBEDTLS_ENTROPY_HARDWARE_ALT

#include "main.h"
#include "string.h"
#include "stm32h7xx_hal.h"
#include "mbedtls/entropy.h"

extern RNG_HandleTypeDef hrng;

int mbedtls_hardware_poll( void *Data, unsigned char *Output, size_t Len, size_t *oLen )
{
  size_t produced = 0U;
  uint32_t randomValue;

  (void)Data;
  if ((Output == NULL) || (oLen == NULL))
  {
    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  }

  *oLen = 0U;
  while (produced < Len)
  {
    size_t remaining = Len - produced;
    size_t chunk = remaining < sizeof(randomValue)
                       ? remaining
                       : sizeof(randomValue);
    if (HAL_RNG_GenerateRandomNumber(&hrng, &randomValue) != HAL_OK)
    {
      return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    memcpy(Output + produced, &randomValue, chunk);
    produced += chunk;
  }

  *oLen = produced;
  return 0;
}

int MbedtlsPlatform_Random(void *context,
                           unsigned char *output,
                           size_t length)
{
  size_t produced = 0U;
  int result = mbedtls_hardware_poll(context, output, length, &produced);
  return (result == 0) && (produced == length)
             ? 0
             : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

#endif /*MBEDTLS_ENTROPY_HARDWARE_ALT*/
