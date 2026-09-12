#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>

#include "adc_bsp.h"

static adc_cali_handle_t cali_handle = NULL;
static adc_oneshot_unit_handle_t adc1_handle = NULL;
static bool cali_enabled = false;
static bool adc_initialized = false;

void Adc_PortInit(void) {
    if (adc_initialized) return;

    // 1. Try curve fitting calibration
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = ADC_ATTEN_DB_12;
    cali_config.bitwidth = ADC_BITWIDTH_12;
    esp_err_t cal_err = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle);
    if (cal_err == ESP_OK) {
        cali_enabled = true;
    } else {
        cali_enabled = false;
        Serial.printf("[ADC] Calibration warning: err %d, using uncalibrated fallback\n", cal_err);
    }

    // 2. Init ADC Unit 1
    adc_oneshot_unit_init_cfg_t init_config1 = {};
    init_config1.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&init_config1, &adc1_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t config = {};
        config.bitwidth = ADC_BITWIDTH_12;
        config.atten = ADC_ATTEN_DB_12;
        if (adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config) == ESP_OK) {
            adc_initialized = true;
            Serial.println("[OK] ADC Batterieueberwachung initialisiert (GPIO 4 / ADC1_CH3).");
        }
    }
}

float Adc_GetBatteryVoltage(int *data) {
    if (!adc_initialized) return 0.0f;

    const int SAMPLES = 8;
    int sum = 0;
    int valid_samples = 0;

    for (int i = 0; i < SAMPLES; i++) {
        int val = 0;
        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &val) == ESP_OK) {
            sum += val;
            valid_samples++;
        }
        delayMicroseconds(50);
    }

    if (valid_samples == 0) return 0.0f;
    int raw_avg = sum / valid_samples;
    if (data) *data = raw_avg;

    float vol = 0.0f;
    if (cali_enabled && cali_handle) {
        int tage_mv = 0;
        if (adc_cali_raw_to_voltage(cali_handle, raw_avg, &tage_mv) == ESP_OK) {
            vol = (float)tage_mv * 3.0f / 1000.0f;
        }
    } else {
        // Fallback: 3.3V full-scale at 12dB attenuation is approx 3.1V..3.3V
        vol = ((float)raw_avg / 4095.0f) * 3.3f * 3.0f;
    }

    return vol;
}

uint8_t Adc_GetBatteryLevel(float vol) {
    if (vol < 0.0f) {
        vol = Adc_GetBatteryVoltage(NULL);
    }
    // Li-Ion (18650 / 3.7V) realistic discharge mapping
    if (vol <= 3.20f) return 0;
    if (vol >= 4.18f) return 100;
    if (vol >= 4.00f) return (uint8_t)(80.0f + ((vol - 4.00f) / 0.18f) * 20.0f);
    if (vol >= 3.80f) return (uint8_t)(50.0f + ((vol - 3.80f) / 0.20f) * 30.0f);
    if (vol >= 3.60f) return (uint8_t)(20.0f + ((vol - 3.60f) / 0.20f) * 30.0f);
    return (uint8_t)(((vol - 3.20f) / 0.40f) * 20.0f);
}