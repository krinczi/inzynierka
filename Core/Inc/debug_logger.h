#ifndef INC_DEBUG_LOGGER_H_
#define INC_DEBUG_LOGGER_H_

#include <stdint.h>
#include "stm32h7xx_hal.h"

typedef enum
{
    DEBUG_SRC_SYSTEM = 0,
    DEBUG_SRC_NET,
    DEBUG_SRC_TLS,
    DEBUG_SRC_TCP,
    DEBUG_SRC_ETH,
    DEBUG_SRC_COUNT
} DebugSource_t;

void DebugLogger_Init(UART_HandleTypeDef *uart);
void DebugLogger_NotifyUartTxComplete(UART_HandleTypeDef *uart);
void DebugLogger_NotifyUartError(UART_HandleTypeDef *uart);

void Debug_LogLine(DebugSource_t source, const char *text);
void Debug_Logf(DebugSource_t source, const char *fmt, ...);

uint32_t DebugLogger_GetDroppedCount(DebugSource_t source);

#endif /* INC_DEBUG_LOGGER_H_ */
