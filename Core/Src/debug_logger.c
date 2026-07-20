#include "debug_logger.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DEBUG_LOG_QUEUE_DEPTH       48U
#define DEBUG_LOG_TEXT_MAX          160U
#define DEBUG_LOG_TX_MAX            224U
#define DEBUG_LOG_TASK_STACK_WORDS  768U
#define DEBUG_LOG_TASK_PRIORITY     (tskIDLE_PRIORITY + 3U)
#define DEBUG_LOG_UART_TX_TIMEOUT_MS 500U

typedef struct
{
    uint32_t tick;
    uint16_t len;
    char text[DEBUG_LOG_TEXT_MAX];
} DebugLogMessage_t;

static UART_HandleTypeDef *debug_uart;
static QueueHandle_t debug_queues[DEBUG_SRC_COUNT];
static TaskHandle_t debug_task_handle;

static volatile uint8_t debug_uart_busy;
static volatile uint32_t debug_dropped[DEBUG_SRC_COUNT];
static volatile uint32_t debug_uart_errors;

static uint32_t debug_reported_dropped[DEBUG_SRC_COUNT];
static uint32_t debug_reported_uart_errors;
static uint8_t debug_next_queue;
static uint8_t debug_tx_buf[DEBUG_LOG_TX_MAX];

static const char *const debug_source_tags[DEBUG_SRC_COUNT] =
{
    "SYS",
    "NET",
    "TLS",
    "TCP",
    "ETH"
};

static uint8_t DebugLogger_SourceIsValid(DebugSource_t source)
{
    return ((uint32_t)source < (uint32_t)DEBUG_SRC_COUNT) ? 1U : 0U;
}

static void DebugLogger_AppendChar(char *buffer,
                                   size_t capacity,
                                   size_t *length,
                                   char value)
{
    if ((*length + 1U) < capacity)
    {
        buffer[*length] = value;
        (*length)++;
        buffer[*length] = '\0';
    }
}

static void DebugLogger_AppendBytes(char *buffer,
                                    size_t capacity,
                                    size_t *length,
                                    const char *value,
                                    size_t value_length)
{
    size_t index;

    if (value == NULL)
    {
        value = "(null)";
        value_length = sizeof("(null)") - 1U;
    }

    for (index = 0U; index < value_length; index++)
    {
        DebugLogger_AppendChar(buffer, capacity, length, value[index]);
    }
}

static void DebugLogger_AppendString(char *buffer,
                                     size_t capacity,
                                     size_t *length,
                                     const char *value)
{
    size_t value_length = 0U;

    if (value == NULL)
    {
        value = "(null)";
    }

    while (value[value_length] != '\0')
    {
        value_length++;
    }

    DebugLogger_AppendBytes(buffer, capacity, length, value, value_length);
}

static void DebugLogger_AppendUnsigned(char *buffer,
                                       size_t capacity,
                                       size_t *length,
                                       uint32_t value)
{
    char digits[10];
    size_t digit_count = 0U;

    do
    {
        digits[digit_count] = (char)('0' + (value % 10U));
        digit_count++;
        value /= 10U;
    }
    while ((value != 0U) && (digit_count < sizeof(digits)));

    while (digit_count > 0U)
    {
        digit_count--;
        DebugLogger_AppendChar(buffer, capacity, length, digits[digit_count]);
    }
}

static uint16_t DebugLogger_FormatCounterMessage(char *buffer,
                                                 size_t capacity,
                                                 const char *prefix,
                                                 uint32_t value,
                                                 const char *suffix)
{
    size_t length = 0U;

    if (capacity == 0U)
    {
        return 0U;
    }

    buffer[0] = '\0';
    DebugLogger_AppendString(buffer, capacity, &length, prefix);
    DebugLogger_AppendUnsigned(buffer, capacity, &length, value);
    DebugLogger_AppendString(buffer, capacity, &length, suffix);

    return (uint16_t)length;
}

static size_t DebugLogger_CopyLine(char *dst, size_t dst_len, const char *src)
{
    size_t len = 0U;

    if (dst_len == 0U)
    {
        return 0U;
    }

    if (src == NULL)
    {
        src = "(null)";
    }

    while ((src[len] != '\0') && (len < (dst_len - 1U)))
    {
        dst[len] = src[len];
        len++;
    }

    dst[len] = '\0';

    while ((len > 0U) &&
           ((dst[len - 1U] == '\r') || (dst[len - 1U] == '\n')))
    {
        len--;
        dst[len] = '\0';
    }

    return len;
}

static void DebugLogger_NotifyTask(void)
{
    if ((debug_task_handle != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        (void)xTaskNotifyGive(debug_task_handle);
    }
}

static uint8_t DebugLogger_TryReceive(DebugSource_t *source,
                                      DebugLogMessage_t *message)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)DEBUG_SRC_COUNT; i++)
    {
        uint8_t index = (uint8_t)((debug_next_queue + i) % (uint8_t)DEBUG_SRC_COUNT);

        if ((debug_queues[index] != NULL) &&
            (xQueueReceive(debug_queues[index], message, 0U) == pdPASS))
        {
            debug_next_queue = (uint8_t)((index + 1U) % (uint8_t)DEBUG_SRC_COUNT);
            *source = (DebugSource_t)index;
            return 1U;
        }
    }

    return 0U;
}

static void DebugLogger_TransmitBuffer(size_t len)
{
    HAL_StatusTypeDef status;
    uint32_t start_tick;

    if ((debug_uart == NULL) || (len == 0U))
    {
        return;
    }

    if (debug_uart->gState != HAL_UART_STATE_READY)
    {
        (void)HAL_UART_AbortTransmit(debug_uart);
        debug_uart_busy = 0U;
        debug_uart_errors++;
        return;
    }

    debug_uart_busy = 1U;
    start_tick = HAL_GetTick();
    status = HAL_UART_Transmit_IT(debug_uart, debug_tx_buf, (uint16_t)len);

    if (status != HAL_OK)
    {
        (void)HAL_UART_AbortTransmit(debug_uart);
        debug_uart_busy = 0U;
        debug_uart_errors++;
        return;
    }

    while (debug_uart_busy != 0U)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20U));

        if (debug_uart->gState == HAL_UART_STATE_READY)
        {
            debug_uart_busy = 0U;
        }

        if ((HAL_GetTick() - start_tick) > DEBUG_LOG_UART_TX_TIMEOUT_MS)
        {
            (void)HAL_UART_AbortTransmit(debug_uart);
            debug_uart_busy = 0U;
            debug_uart_errors++;
        }
    }
}

static void DebugLogger_TransmitLine(DebugSource_t source,
                                     const DebugLogMessage_t *message)
{
    size_t length = 0U;
    size_t text_length = message->len;

    if (text_length >= sizeof(message->text))
    {
        text_length = sizeof(message->text) - 1U;
    }

    debug_tx_buf[0] = '\0';
    DebugLogger_AppendChar((char *)debug_tx_buf, sizeof(debug_tx_buf), &length, '[');
    DebugLogger_AppendUnsigned((char *)debug_tx_buf,
                               sizeof(debug_tx_buf),
                               &length,
                               message->tick);
    DebugLogger_AppendString((char *)debug_tx_buf, sizeof(debug_tx_buf), &length, "][");
    DebugLogger_AppendString((char *)debug_tx_buf,
                             sizeof(debug_tx_buf),
                             &length,
                             debug_source_tags[(uint32_t)source]);
    DebugLogger_AppendString((char *)debug_tx_buf, sizeof(debug_tx_buf), &length, "] ");
    DebugLogger_AppendBytes((char *)debug_tx_buf,
                            sizeof(debug_tx_buf),
                            &length,
                            message->text,
                            text_length);
    DebugLogger_AppendString((char *)debug_tx_buf, sizeof(debug_tx_buf), &length, "\r\n");

    DebugLogger_TransmitBuffer(length);
}

static void DebugLogger_ReportDropped(void)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)DEBUG_SRC_COUNT; i++)
    {
        uint32_t current = debug_dropped[i];

        if (current != debug_reported_dropped[i])
        {
            DebugLogMessage_t message;
            uint32_t delta = current - debug_reported_dropped[i];

            debug_reported_dropped[i] = current;
            message.tick = HAL_GetTick();
            message.len = DebugLogger_FormatCounterMessage(
                message.text,
                sizeof(message.text),
                "debug queue dropped ",
                delta,
                " log(s)");

            DebugLogger_TransmitLine((DebugSource_t)i, &message);
        }
    }

    if (debug_uart_errors != debug_reported_uart_errors)
    {
        DebugLogMessage_t message;
        uint32_t delta = debug_uart_errors - debug_reported_uart_errors;

        debug_reported_uart_errors = debug_uart_errors;
        message.tick = HAL_GetTick();
        message.len = DebugLogger_FormatCounterMessage(
            message.text,
            sizeof(message.text),
            "uart transmit error/drop ",
            delta,
            " time(s)");

        DebugLogger_TransmitLine(DEBUG_SRC_SYSTEM, &message);
    }
}

static void DebugLogger_Task(void *argument)
{
    DebugLogMessage_t message;
    DebugSource_t source;

    (void)argument;

    for (;;)
    {
        if (DebugLogger_TryReceive(&source, &message) != 0U)
        {
            DebugLogger_ReportDropped();
            DebugLogger_TransmitLine(source, &message);
            continue;
        }

        DebugLogger_ReportDropped();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50U));
    }
}

void DebugLogger_Init(UART_HandleTypeDef *uart)
{
    uint32_t i;

    debug_uart = uart;

    for (i = 0U; i < (uint32_t)DEBUG_SRC_COUNT; i++)
    {
        if (debug_queues[i] == NULL)
        {
            debug_queues[i] = xQueueCreate(DEBUG_LOG_QUEUE_DEPTH,
                                           sizeof(DebugLogMessage_t));
        }
    }

    if (debug_task_handle == NULL)
    {
        (void)xTaskCreate(DebugLogger_Task,
                          "DebugTask",
                          DEBUG_LOG_TASK_STACK_WORDS,
                          NULL,
                          DEBUG_LOG_TASK_PRIORITY,
                          &debug_task_handle);
    }
}

void Debug_LogLine(DebugSource_t source, const char *text)
{
    DebugLogMessage_t message;
    uint32_t index = (uint32_t)source;

    if (DebugLogger_SourceIsValid(source) == 0U)
    {
        return;
    }

    message.tick = HAL_GetTick();
    message.len = (uint16_t)DebugLogger_CopyLine(message.text,
                                                 sizeof(message.text),
                                                 text);

    if ((debug_queues[index] == NULL) ||
        (xQueueSend(debug_queues[index], &message, 0U) != pdPASS))
    {
        debug_dropped[index]++;
        return;
    }

    DebugLogger_NotifyTask();
}

void Debug_Logf(DebugSource_t source, const char *fmt, ...)
{
    char text[DEBUG_LOG_TEXT_MAX];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    text[sizeof(text) - 1U] = '\0';
    Debug_LogLine(source, text);
}

uint32_t DebugLogger_GetDroppedCount(DebugSource_t source)
{
    if (DebugLogger_SourceIsValid(source) == 0U)
    {
        return 0U;
    }

    return debug_dropped[(uint32_t)source];
}

void DebugLogger_NotifyUartTxComplete(UART_HandleTypeDef *uart)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((uart == debug_uart) && (debug_task_handle != NULL))
    {
        debug_uart_busy = 0U;
        vTaskNotifyGiveFromISR(debug_task_handle, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

void DebugLogger_NotifyUartError(UART_HandleTypeDef *uart)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((uart == debug_uart) && (debug_task_handle != NULL))
    {
        debug_uart_busy = 0U;
        debug_uart_errors++;
        vTaskNotifyGiveFromISR(debug_task_handle, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart)
{
    DebugLogger_NotifyUartTxComplete(uart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    DebugLogger_NotifyUartError(uart);
}
