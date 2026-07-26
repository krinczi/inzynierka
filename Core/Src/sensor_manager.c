#include "sensor_manager.h"

#include "main.h"
#include "stm32h7xx_hal_adc.h"
#include "stm32h7xx_hal_adc_ex.h"
#include "stm32h7xx_ll_adc.h"

#include <string.h>

#define SENSOR_ADC_TIMEOUT_MS               20U
#define SENSOR_ADC_SAMPLE_COUNT              8U
#define SENSOR_TEMPERATURE_MIN_MILLI_C  (-40000L)
#define SENSOR_TEMPERATURE_MAX_MILLI_C  (150000L)
#define SENSOR_VDDA_MIN_MV                 1800U
#define SENSOR_VDDA_MAX_MV                 3600U

static ADC_HandleTypeDef sensor_adc;
static uint8_t sensor_initialized;

static int SensorManager_ReadChannel(uint32_t channel, uint32_t *average)
{
    ADC_ChannelConfTypeDef config = { 0 };
    uint64_t sum = 0U;
    uint32_t index;

    if (average == NULL)
    {
        return -1;
    }

    config.Channel = channel;
    config.Rank = ADC_REGULAR_RANK_1;
    config.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;
    config.SingleDiff = ADC_SINGLE_ENDED;
    config.OffsetNumber = ADC_OFFSET_NONE;
    config.Offset = 0;
    config.OffsetRightShift = DISABLE;
    config.OffsetSignedSaturation = DISABLE;

    if (HAL_ADC_ConfigChannel(&sensor_adc, &config) != HAL_OK)
    {
        return -1;
    }

    for (index = 0U; index < SENSOR_ADC_SAMPLE_COUNT; index++)
    {
        if ((HAL_ADC_Start(&sensor_adc) != HAL_OK) ||
            (HAL_ADC_PollForConversion(&sensor_adc,
                                       SENSOR_ADC_TIMEOUT_MS) != HAL_OK))
        {
            (void)HAL_ADC_Stop(&sensor_adc);
            return -1;
        }

        sum += HAL_ADC_GetValue(&sensor_adc);
        if (HAL_ADC_Stop(&sensor_adc) != HAL_OK)
        {
            return -1;
        }
    }

    *average = (uint32_t)(sum / SENSOR_ADC_SAMPLE_COUNT);
    return 0;
}

int SensorManager_Init(void)
{
    if (sensor_initialized != 0U)
    {
        return 0;
    }

    memset(&sensor_adc, 0, sizeof(sensor_adc));
    sensor_adc.Instance = ADC3;
    sensor_adc.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV2;
    sensor_adc.Init.Resolution = ADC_RESOLUTION_16B;
    sensor_adc.Init.ScanConvMode = DISABLE;
    sensor_adc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    sensor_adc.Init.LowPowerAutoWait = DISABLE;
    sensor_adc.Init.ContinuousConvMode = DISABLE;
    sensor_adc.Init.NbrOfConversion = 1;
    sensor_adc.Init.DiscontinuousConvMode = DISABLE;
    sensor_adc.Init.NbrOfDiscConversion = 1;
    sensor_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    sensor_adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    sensor_adc.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    sensor_adc.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    sensor_adc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    sensor_adc.Init.OversamplingMode = DISABLE;

    if ((HAL_ADC_Init(&sensor_adc) != HAL_OK) ||
        (HAL_ADCEx_Calibration_Start(&sensor_adc,
                                    ADC_CALIB_OFFSET_LINEARITY,
                                    ADC_SINGLE_ENDED) != HAL_OK))
    {
        (void)HAL_ADC_DeInit(&sensor_adc);
        return -1;
    }

    sensor_initialized = 1U;
    return 0;
}

int SensorManager_Read(SensorSnapshot *snapshot)
{
    uint32_t vref_raw;
    uint32_t temperature_raw;
    uint32_t vdda_mv;
    int32_t temperature_c;

    if (snapshot == NULL)
    {
        return -1;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->sampled_at_uptime_ms = HAL_GetTick();

    if (sensor_initialized == 0U)
    {
        snapshot->status = SENSOR_STATUS_NOT_INITIALIZED;
        return -1;
    }

    if ((SensorManager_ReadChannel(ADC_CHANNEL_VREFINT, &vref_raw) != 0) ||
        (vref_raw == 0U))
    {
        snapshot->status = SENSOR_STATUS_ADC_ERROR;
        return -1;
    }

    vdda_mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(vref_raw, LL_ADC_RESOLUTION_16B);
    if ((vdda_mv >= SENSOR_VDDA_MIN_MV) && (vdda_mv <= SENSOR_VDDA_MAX_MV))
    {
        snapshot->supply_millivolts = vdda_mv;
        snapshot->valid_mask |= SENSOR_VALID_VOLTAGE;
    }

    if (SensorManager_ReadChannel(ADC_CHANNEL_TEMPSENSOR,
                                  &temperature_raw) == 0)
    {
        temperature_c =
            __LL_ADC_CALC_TEMPERATURE(vdda_mv,
                                     temperature_raw,
                                     LL_ADC_RESOLUTION_16B);
        snapshot->temperature_milli_celsius = temperature_c * 1000L;
        if ((snapshot->temperature_milli_celsius >=
             SENSOR_TEMPERATURE_MIN_MILLI_C) &&
            (snapshot->temperature_milli_celsius <=
             SENSOR_TEMPERATURE_MAX_MILLI_C))
        {
            snapshot->valid_mask |= SENSOR_VALID_TEMPERATURE;
        }
    }

    if ((SensorPlatform_ReadCurrentMicroamps(&snapshot->current_microamps) == 0) &&
        (snapshot->current_microamps >= 0))
    {
        snapshot->valid_mask |= SENSOR_VALID_CURRENT;
    }

    if ((SensorPlatform_ReadHumidityMilliPercent(
             &snapshot->humidity_milli_percent) == 0) &&
        (snapshot->humidity_milli_percent <= 100000U))
    {
        snapshot->valid_mask |= SENSOR_VALID_HUMIDITY;
    }

    if ((SensorPlatform_ReadSignalPercent(&snapshot->signal_percent) == 0) &&
        (snapshot->signal_percent <= 100U))
    {
        snapshot->valid_mask |= SENSOR_VALID_SIGNAL;
    }

    snapshot->status =
        ((snapshot->valid_mask &
          (SENSOR_VALID_TEMPERATURE | SENSOR_VALID_VOLTAGE)) ==
         (SENSOR_VALID_TEMPERATURE | SENSOR_VALID_VOLTAGE))
            ? SENSOR_STATUS_OK
            : SENSOR_STATUS_PARTIAL;
    return (snapshot->valid_mask != 0U) ? 0 : -1;
}

__weak int SensorPlatform_ReadCurrentMicroamps(int32_t *value)
{
    (void)value;
    return -1;
}

__weak int SensorPlatform_ReadHumidityMilliPercent(uint32_t *value)
{
    (void)value;
    return -1;
}

__weak int SensorPlatform_ReadSignalPercent(uint32_t *value)
{
    (void)value;
    return -1;
}

const char *SensorManager_StatusName(SensorStatus status)
{
    switch (status)
    {
        case SENSOR_STATUS_OK:
            return "OK";
        case SENSOR_STATUS_PARTIAL:
            return "PARTIAL";
        case SENSOR_STATUS_ADC_ERROR:
            return "ADC_ERROR";
        case SENSOR_STATUS_NOT_INITIALIZED:
        default:
            return "NOT_INITIALIZED";
    }
}
