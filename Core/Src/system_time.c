#include "system_time.h"

#include "debug_logger.h"
#include "main.h"

#include "cmsis_os.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include "stm32h7xx_hal_rtc.h"
#include "stm32h7xx_hal_rtc_ex.h"

#include <errno.h>
#include <string.h>

#define SYSTEM_TIME_BACKUP_MAGIC            0x54494D45UL
#define SYSTEM_TIME_BACKUP_MAGIC_REGISTER   RTC_BKP_DR0
#define SYSTEM_TIME_NTP_PACKET_SIZE         48U
#define SYSTEM_TIME_NTP_UNIX_OFFSET         2208988800UL
#define SYSTEM_TIME_MIN_UNIX                1704067200UL /* 2024-01-01 */
#define SYSTEM_TIME_MAX_UNIX                4102444799UL /* 2099-12-31 */
#define SYSTEM_TIME_NTP_PORT                123U
#define SYSTEM_TIME_NTP_TIMEOUT_MS          3000U
#define SYSTEM_TIME_DNS_START_TIMEOUT_MS    2000U
#define SYSTEM_TIME_DNS_TIMEOUT_MS          8000U
#define SYSTEM_TIME_REBASE_INTERVAL_MS      43200000U

static RTC_HandleTypeDef system_rtc;
static SystemTimeState system_time_state = SYSTEM_TIME_INVALID;
static uint32_t system_time_base_unix;
static uint32_t system_time_base_tick;
static volatile uint8_t system_time_dns_done;
static volatile uint8_t system_time_dns_start_done;
static volatile err_t system_time_dns_error;
static volatile err_t system_time_dns_start_error;
static ip_addr_t system_time_dns_address;
static char system_time_dns_hostname[256];

static const char *const system_time_servers[] = {
    "time.cloudflare.com",
    "time.google.com",
    "pool.ntp.org"
};

static void SystemTime_DnsFound(const char *name,
                                const ip_addr_t *address,
                                void *argument)
{
    (void)name;
    (void)argument;

    if ((address != NULL) && IP_IS_V4(address))
    {
        system_time_dns_address = *address;
        system_time_dns_error = ERR_OK;
    }
    else
    {
        system_time_dns_error = ERR_VAL;
    }
    system_time_dns_done = 1U;
}

static void SystemTime_DnsStartOnTcpipThread(void *argument)
{
    ip_addr_t address;
    err_t result;

    (void)argument;
    result = dns_gethostbyname(system_time_dns_hostname,
                               &address,
                               SystemTime_DnsFound,
                               NULL);
    system_time_dns_start_error = result;
    if (result == ERR_OK)
    {
        if (IP_IS_V4(&address))
        {
            system_time_dns_address = address;
            system_time_dns_error = ERR_OK;
        }
        else
        {
            system_time_dns_error = ERR_VAL;
        }
        system_time_dns_done = 1U;
    }
    else if (result != ERR_INPROGRESS)
    {
        system_time_dns_error = result;
        system_time_dns_done = 1U;
    }
    system_time_dns_start_done = 1U;
}

static int SystemTime_ResolveIpv4(const char *hostname,
                                  ip_addr_t *address)
{
    uint32_t started_at;
    err_t result;

    if ((hostname == NULL) || (address == NULL))
    {
        return -1;
    }

    system_time_dns_done = 0U;
    system_time_dns_start_done = 0U;
    system_time_dns_error = ERR_INPROGRESS;
    system_time_dns_start_error = ERR_INPROGRESS;
    memset(&system_time_dns_address, 0, sizeof(system_time_dns_address));
    strncpy(system_time_dns_hostname,
            hostname,
            sizeof(system_time_dns_hostname) - 1U);
    system_time_dns_hostname[sizeof(system_time_dns_hostname) - 1U] = '\0';

    started_at = HAL_GetTick();
    result = tcpip_callback(SystemTime_DnsStartOnTcpipThread, NULL);
    if (result != ERR_OK)
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: DNS dispatch failed server=%s err=%d",
                   hostname,
                   (int)result);
        return -1;
    }

    while (system_time_dns_start_done == 0U)
    {
        if ((HAL_GetTick() - started_at) >=
            SYSTEM_TIME_DNS_START_TIMEOUT_MS)
        {
            Debug_Logf(DEBUG_SRC_NET,
                       "TIME: DNS start timeout server=%s",
                       hostname);
            return -1;
        }
        osDelay(10U);
    }

    if ((system_time_dns_start_error != ERR_OK) &&
        (system_time_dns_start_error != ERR_INPROGRESS))
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: DNS start failed server=%s err=%d",
                   hostname,
                   (int)system_time_dns_start_error);
        return -1;
    }

    while (system_time_dns_done == 0U)
    {
        if ((HAL_GetTick() - started_at) >= SYSTEM_TIME_DNS_TIMEOUT_MS)
        {
            Debug_Logf(DEBUG_SRC_NET,
                       "TIME: DNS timeout server=%s",
                       hostname);
            return -1;
        }
        osDelay(50U);
    }

    if ((system_time_dns_error != ERR_OK) ||
        !IP_IS_V4(&system_time_dns_address))
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: DNS failed server=%s err=%d",
                   hostname,
                   (int)system_time_dns_error);
        return -1;
    }

    *address = system_time_dns_address;
    return 0;
}

static int SystemTime_IsLeapYear(uint32_t year)
{
    return ((year % 4U) == 0U) &&
           (((year % 100U) != 0U) || ((year % 400U) == 0U));
}

static uint32_t SystemTime_DaysBeforeYear(uint32_t year)
{
    uint32_t previous = year - 1U;
    return (365U * (year - 1970U)) +
           (previous / 4U - 1969U / 4U) -
           (previous / 100U - 1969U / 100U) +
           (previous / 400U - 1969U / 400U);
}

static uint32_t SystemTime_DaysBeforeMonth(uint32_t year, uint32_t month)
{
    static const uint16_t cumulative_days[] = {
        0U, 31U, 59U, 90U, 120U, 151U,
        181U, 212U, 243U, 273U, 304U, 334U
    };
    uint32_t days;

    if ((month < 1U) || (month > 12U))
    {
        return UINT32_MAX;
    }

    days = cumulative_days[month - 1U];
    if ((month > 2U) && SystemTime_IsLeapYear(year))
    {
        days++;
    }
    return days;
}

static int SystemTime_CalendarToUnix(const RTC_DateTypeDef *date,
                                     const RTC_TimeTypeDef *time,
                                     uint32_t *unix_time)
{
    uint32_t year;
    uint32_t month_days;
    uint32_t days;
    uint64_t seconds;

    if ((date == NULL) || (time == NULL) || (unix_time == NULL))
    {
        return -1;
    }

    year = 2000U + date->Year;
    month_days = SystemTime_DaysBeforeMonth(year, date->Month);
    if ((year < 2024U) || (year > 2099U) ||
        (month_days == UINT32_MAX) ||
        (date->Date < 1U) || (date->Date > 31U) ||
        (time->Hours > 23U) || (time->Minutes > 59U) ||
        (time->Seconds > 59U))
    {
        return -1;
    }

    days = SystemTime_DaysBeforeYear(year) +
           month_days + (uint32_t)date->Date - 1U;
    seconds = ((uint64_t)days * 86400ULL) +
              ((uint64_t)time->Hours * 3600ULL) +
              ((uint64_t)time->Minutes * 60ULL) +
              time->Seconds;
    if ((seconds < SYSTEM_TIME_MIN_UNIX) ||
        (seconds > SYSTEM_TIME_MAX_UNIX))
    {
        return -1;
    }

    *unix_time = (uint32_t)seconds;
    return 0;
}

static int SystemTime_UnixToCalendar(uint32_t unix_time,
                                     RTC_DateTypeDef *date,
                                     RTC_TimeTypeDef *time)
{
    static const uint8_t month_lengths[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    uint32_t days;
    uint32_t seconds;
    uint32_t year = 1970U;
    uint32_t month = 1U;

    if ((date == NULL) || (time == NULL) ||
        (unix_time < SYSTEM_TIME_MIN_UNIX) ||
        (unix_time > SYSTEM_TIME_MAX_UNIX))
    {
        return -1;
    }

    days = unix_time / 86400U;
    seconds = unix_time % 86400U;
    while (days >= (SystemTime_IsLeapYear(year) ? 366U : 365U))
    {
        days -= SystemTime_IsLeapYear(year) ? 366U : 365U;
        year++;
    }

    while (month <= 12U)
    {
        uint32_t length = month_lengths[month - 1U];
        if ((month == 2U) && SystemTime_IsLeapYear(year))
        {
            length++;
        }
        if (days < length)
        {
            break;
        }
        days -= length;
        month++;
    }

    memset(date, 0, sizeof(*date));
    memset(time, 0, sizeof(*time));
    date->Year = (uint8_t)(year - 2000U);
    date->Month = (uint8_t)month;
    date->Date = (uint8_t)(days + 1U);
    date->WeekDay = (uint8_t)(((unix_time / 86400U + 3U) % 7U) + 1U);
    time->Hours = (uint8_t)(seconds / 3600U);
    seconds %= 3600U;
    time->Minutes = (uint8_t)(seconds / 60U);
    time->Seconds = (uint8_t)(seconds % 60U);
    time->DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    time->StoreOperation = RTC_STOREOPERATION_RESET;
    return 0;
}

static int SystemTime_ReadRtc(uint32_t *unix_time)
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;

    if ((HAL_RTC_GetTime(&system_rtc, &time, RTC_FORMAT_BIN) != HAL_OK) ||
        (HAL_RTC_GetDate(&system_rtc, &date, RTC_FORMAT_BIN) != HAL_OK))
    {
        return -1;
    }
    return SystemTime_CalendarToUnix(&date, &time, unix_time);
}

static int SystemTime_SetRtc(uint32_t unix_time)
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;

    if (SystemTime_UnixToCalendar(unix_time, &date, &time) != 0)
    {
        return -1;
    }

    if ((HAL_RTC_SetTime(&system_rtc, &time, RTC_FORMAT_BIN) != HAL_OK) ||
        (HAL_RTC_SetDate(&system_rtc, &date, RTC_FORMAT_BIN) != HAL_OK))
    {
        return -1;
    }

    HAL_RTCEx_BKUPWrite(&system_rtc,
                        SYSTEM_TIME_BACKUP_MAGIC_REGISTER,
                        SYSTEM_TIME_BACKUP_MAGIC);
    system_time_base_unix = unix_time;
    system_time_base_tick = HAL_GetTick();
    return 0;
}

static int SystemTime_EnableRtcClock(uint32_t *source)
{
    RCC_OscInitTypeDef oscillator = { 0 };
    uint32_t selected = __HAL_RCC_GET_RTC_SOURCE();

    HAL_PWR_EnableBkUpAccess();

    if (selected == RCC_RTCCLKSOURCE_LSE)
    {
        oscillator.OscillatorType = RCC_OSCILLATORTYPE_LSE;
        oscillator.LSEState = RCC_LSE_ON;
        oscillator.PLL.PLLState = RCC_PLL_NONE;
        if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
        {
            return -1;
        }
    }
    else if (selected == RCC_RTCCLKSOURCE_LSI)
    {
        oscillator.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        oscillator.LSIState = RCC_LSI_ON;
        oscillator.PLL.PLLState = RCC_PLL_NONE;
        if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
        {
            return -1;
        }
    }
    else
    {
        oscillator.OscillatorType = RCC_OSCILLATORTYPE_LSE;
        oscillator.LSEState = RCC_LSE_ON;
        oscillator.PLL.PLLState = RCC_PLL_NONE;
        if (HAL_RCC_OscConfig(&oscillator) == HAL_OK)
        {
            selected = RCC_RTCCLKSOURCE_LSE;
        }
        else
        {
            memset(&oscillator, 0, sizeof(oscillator));
            oscillator.OscillatorType = RCC_OSCILLATORTYPE_LSI;
            oscillator.LSIState = RCC_LSI_ON;
            oscillator.PLL.PLLState = RCC_PLL_NONE;
            if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
            {
                return -1;
            }
            selected = RCC_RTCCLKSOURCE_LSI;
        }

        __HAL_RCC_RTC_CONFIG(selected);
    }

    __HAL_RCC_RTC_ENABLE();
    *source = selected;
    return 0;
}

int SystemTime_Init(void)
{
    uint32_t source;
    uint32_t unix_time = 0U;

    memset(&system_rtc, 0, sizeof(system_rtc));
    system_time_state = SYSTEM_TIME_INVALID;
    system_time_base_unix = 0U;
    system_time_base_tick = HAL_GetTick();

    if (SystemTime_EnableRtcClock(&source) != 0)
    {
        return -1;
    }

    system_rtc.Instance = RTC;
    system_rtc.Init.HourFormat = RTC_HOURFORMAT_24;
    system_rtc.Init.AsynchPrediv = 127U;
    system_rtc.Init.SynchPrediv =
        (source == RCC_RTCCLKSOURCE_LSE) ? 255U : 249U;
    system_rtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    system_rtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    system_rtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    system_rtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;

    if (HAL_RTC_Init(&system_rtc) != HAL_OK)
    {
        return -1;
    }

    if ((HAL_RTCEx_BKUPRead(&system_rtc,
                            SYSTEM_TIME_BACKUP_MAGIC_REGISTER) ==
         SYSTEM_TIME_BACKUP_MAGIC) &&
        (SystemTime_ReadRtc(&unix_time) == 0))
    {
        system_time_base_unix = unix_time;
        system_time_base_tick = HAL_GetTick();
        system_time_state = SYSTEM_TIME_RTC_HOLDOVER;
    }

    return 0;
}

static int SystemTime_QueryServer(const char *hostname, uint32_t *unix_time)
{
    ip_addr_t resolved_address;
    struct sockaddr_in server_address;
    uint8_t request[SYSTEM_TIME_NTP_PACKET_SIZE] = { 0 };
    uint8_t response[SYSTEM_TIME_NTP_PACKET_SIZE];
    uint32_t nonce = HAL_GetTick() ^ HAL_GetUIDw0() ^ HAL_GetUIDw2();
    int socket_fd = -1;
    fd_set read_set;
    struct timeval timeout;
    int selected;
    int received;
    uint32_t ntp_seconds;
    uint8_t mode;
    uint8_t stratum;
    char address_text[IPADDR_STRLEN_MAX];

    if ((unix_time == NULL) ||
        (SystemTime_ResolveIpv4(hostname, &resolved_address) != 0))
    {
        return -1;
    }

    ipaddr_ntoa_r(&resolved_address,
                  address_text,
                  sizeof(address_text));
    Debug_Logf(DEBUG_SRC_NET,
               "TIME: NTP resolved server=%s address=%s",
               hostname,
               address_text);

    request[0] = 0x23U; /* LI=0, NTPv4, client mode. */
    request[40] = (uint8_t)(nonce >> 24U);
    request[41] = (uint8_t)(nonce >> 16U);
    request[42] = (uint8_t)(nonce >> 8U);
    request[43] = (uint8_t)nonce;
    request[44] = 0x53U;
    request[45] = 0x54U;
    request[46] = 0x4DU;
    request[47] = 0x33U;

    socket_fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP socket failed server=%s errno=%d",
                   hostname,
                   errno);
        return -1;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = lwip_htons(SYSTEM_TIME_NTP_PORT);
    server_address.sin_addr.s_addr =
        ip4_addr_get_u32(ip_2_ip4(&resolved_address));
    if (lwip_connect(socket_fd,
                     (const struct sockaddr *)&server_address,
                     sizeof(server_address)) != 0)
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP connect failed server=%s errno=%d",
                   hostname,
                   errno);
        (void)lwip_close(socket_fd);
        return -1;
    }

    if (lwip_send(socket_fd,
                  request,
                  sizeof(request),
                  0) != (int)sizeof(request))
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP send failed server=%s errno=%d",
                   hostname,
                   errno);
        (void)lwip_close(socket_fd);
        return -1;
    }

    FD_ZERO(&read_set);
    FD_SET(socket_fd, &read_set);
    timeout.tv_sec = SYSTEM_TIME_NTP_TIMEOUT_MS / 1000U;
    timeout.tv_usec =
        (SYSTEM_TIME_NTP_TIMEOUT_MS % 1000U) * 1000U;
    selected = lwip_select(socket_fd + 1,
                           &read_set,
                           NULL,
                           NULL,
                           &timeout);
    if ((selected <= 0) || !FD_ISSET(socket_fd, &read_set))
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP response timeout server=%s errno=%d",
                   hostname,
                   errno);
        (void)lwip_close(socket_fd);
        return -1;
    }

    received = lwip_recv(socket_fd, response, sizeof(response), 0);
    (void)lwip_close(socket_fd);
    mode = received > 0 ? response[0] & 0x07U : 0U;
    stratum = received > 1 ? response[1] : 0U;
    if ((received < (int)sizeof(response)) ||
        ((response[0] >> 6U) == 3U) ||
        ((mode != 4U) && (mode != 5U)) ||
        (stratum == 0U) || (stratum > 15U) ||
        (memcmp(&response[24], &request[40], 8U) != 0))
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP invalid response server=%s bytes=%d mode=%u stratum=%u",
                   hostname,
                   received,
                   (unsigned int)mode,
                   (unsigned int)stratum);
        return -1;
    }

    ntp_seconds = ((uint32_t)response[40] << 24U) |
                  ((uint32_t)response[41] << 16U) |
                  ((uint32_t)response[42] << 8U) |
                  response[43];
    if (ntp_seconds <= SYSTEM_TIME_NTP_UNIX_OFFSET)
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP invalid timestamp server=%s",
                   hostname);
        return -1;
    }

    *unix_time = ntp_seconds - SYSTEM_TIME_NTP_UNIX_OFFSET;
    if ((*unix_time < SYSTEM_TIME_MIN_UNIX) ||
        (*unix_time > SYSTEM_TIME_MAX_UNIX))
    {
        Debug_Logf(DEBUG_SRC_NET,
                   "TIME: NTP timestamp outside policy server=%s unix=%lu",
                   hostname,
                   (unsigned long)*unix_time);
        return -1;
    }
    return 0;
}

int SystemTime_SyncNetwork(void)
{
    size_t index;
    uint32_t unix_time = 0U;

    for (index = 0U;
         index < (sizeof(system_time_servers) / sizeof(system_time_servers[0]));
         index++)
    {
        if ((SystemTime_QueryServer(system_time_servers[index],
                                    &unix_time) == 0) &&
            (SystemTime_SetRtc(unix_time) == 0))
        {
            system_time_state = SYSTEM_TIME_SNTP_SYNCHRONIZED;
            Debug_Logf(DEBUG_SRC_NET,
                       "TIME: synchronized server=%s unix=%lu",
                       system_time_servers[index],
                       (unsigned long)unix_time);
            return 0;
        }
    }

    Debug_LogLine(DEBUG_SRC_NET, "TIME: SNTP synchronization failed");
    return -1;
}

int SystemTime_GetTimeval(struct timeval *value)
{
    uint32_t now;
    uint32_t elapsed;

    if ((value == NULL) || (system_time_state == SYSTEM_TIME_INVALID))
    {
        errno = ENODATA;
        return -1;
    }

    now = HAL_GetTick();
    elapsed = now - system_time_base_tick;
    if (elapsed >= SYSTEM_TIME_REBASE_INTERVAL_MS)
    {
        uint32_t rtc_unix;
        if (SystemTime_ReadRtc(&rtc_unix) == 0)
        {
            system_time_base_unix = rtc_unix;
            system_time_base_tick = now;
            elapsed = 0U;
        }
    }

    value->tv_sec = (time_t)(system_time_base_unix + (elapsed / 1000U));
    value->tv_usec = (suseconds_t)((elapsed % 1000U) * 1000U);
    return 0;
}

int SystemTime_IsValid(void)
{
    return system_time_state != SYSTEM_TIME_INVALID;
}

SystemTimeState SystemTime_GetState(void)
{
    return system_time_state;
}

const char *SystemTime_StateName(SystemTimeState state)
{
    switch (state)
    {
        case SYSTEM_TIME_RTC_HOLDOVER:
            return "RTC_HOLDOVER";
        case SYSTEM_TIME_SNTP_SYNCHRONIZED:
            return "SNTP_SYNCHRONIZED";
        case SYSTEM_TIME_INVALID:
        default:
            return "INVALID";
    }
}
