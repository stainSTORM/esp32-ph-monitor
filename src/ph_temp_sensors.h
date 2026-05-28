#ifndef PH_TEMP_SENSORS_H
#define PH_TEMP_SENSORS_H

#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BME280.h>
#include <Preferences.h>

// ── Pin configuration ────────────────────────────────────────────────────────
constexpr uint8_t PH_ADC_PIN   = 1;   // pH AOUT → ADC1_CH0
constexpr uint8_t DS18B20_PIN  = 4;   // T1 on pH board → OneWire data
constexpr uint8_t I2C_SDA_PIN  = 8;
constexpr uint8_t I2C_SCL_PIN  = 9;
constexpr uint8_t BME280_ADDR  = 0x76;

// ── pH calibration defaults (overridden by NVS on load) ──────────────────────
// To calibrate: use the calibrate_ph_point Arkitekt function.
constexpr float PH_CAL_V_AT_7_DEFAULT   = 2.50f;
constexpr float PH_CAL_V_AT_4_DEFAULT   = 3.00f;
constexpr float PH_CAL_REF_TEMP         = 25.0f;

// ── ADC oversampling ─────────────────────────────────────────────────────────
constexpr int PH_OVERSAMPLE = 16;

// ── Live calibration state (loaded from NVS, updated by calibrate_ph_point) ──
static float g_calV7 = PH_CAL_V_AT_7_DEFAULT;
static float g_calV4 = PH_CAL_V_AT_4_DEFAULT;

static OneWire           oneWire(DS18B20_PIN);
static DallasTemperature ds18b20(&oneWire);
static bool              ds18b20Ready = false;

static Adafruit_BME280 bme;
static bool            bmeReady = false;

// Nernst-equation temperature compensation.
static float voltageToPH(float v, float tempC = PH_CAL_REF_TEMP)
{
    float slope_ref   = (g_calV4 - g_calV7) / (4.0f - 7.0f);
    float temp_factor = (tempC + 273.15f) / (PH_CAL_REF_TEMP + 273.15f);
    float slope       = slope_ref * temp_factor;
    return constrain(7.0f + (v - g_calV7) / slope, 0.0f, 14.0f);
}

// ── Calibration NVS helpers ───────────────────────────────────────────────────

void loadCalibration()
{
    Preferences prefs;
    prefs.begin("ph_cal", true);
    g_calV7 = prefs.getFloat("v_at_7", PH_CAL_V_AT_7_DEFAULT);
    g_calV4 = prefs.getFloat("v_at_4", PH_CAL_V_AT_4_DEFAULT);
    prefs.end();
    Serial.printf("[CAL] Loaded: V@7=%.4fV  V@4=%.4fV\n", g_calV7, g_calV4);
}

// Saves a single calibration point to NVS and updates the live values.
// bufferPH must be 4.0 or 7.0. Returns false for any other value.
bool saveCalibrationPoint(float bufferPH, float voltage)
{
    if (fabsf(bufferPH - 7.0f) < 0.01f)
        g_calV7 = voltage;
    else if (fabsf(bufferPH - 4.0f) < 0.01f)
        g_calV4 = voltage;
    else
        return false;

    Preferences prefs;
    prefs.begin("ph_cal", false);
    prefs.putFloat("v_at_7", g_calV7);
    prefs.putFloat("v_at_4", g_calV4);
    prefs.end();
    Serial.printf("[CAL] Saved: V@7=%.4fV  V@4=%.4fV\n", g_calV7, g_calV4);
    return true;
}

// Resets calibration to compile-time defaults and clears NVS.
void resetCalibration()
{
    g_calV7 = PH_CAL_V_AT_7_DEFAULT;
    g_calV4 = PH_CAL_V_AT_4_DEFAULT;
    Preferences prefs;
    prefs.begin("ph_cal", false);
    prefs.clear();
    prefs.end();
    Serial.println("[CAL] Reset to defaults");
}

void getCalibrationVoltages(float &v7, float &v4)
{
    v7 = g_calV7;
    v4 = g_calV4;
}

// ── Sensor init ───────────────────────────────────────────────────────────────

void initSensors()
{
    ds18b20.begin();
    int found = ds18b20.getDeviceCount();
    ds18b20Ready = (found > 0);
    if (ds18b20Ready) {
        ds18b20.setResolution(12);
        Serial.printf("[SENSOR] DS18B20 OK  pin=%d  devices=%d\n", DS18B20_PIN, found);
    } else {
        Serial.printf("[SENSOR] DS18B20 NOT found on pin %d\n", DS18B20_PIN);
    }

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    bmeReady = bme.begin(BME280_ADDR);
    if (bmeReady)
        Serial.printf("[SENSOR] BME280 OK  addr=0x%02X\n", BME280_ADDR);
    else
        Serial.printf("[SENSOR] BME280 NOT found at 0x%02X\n", BME280_ADDR);

    analogSetPinAttenuation(PH_ADC_PIN, ADC_11db);
    Serial.printf("[SENSOR] pH ADC  pin=%d  oversample=%d\n", PH_ADC_PIN, PH_OVERSAMPLE);
}

// ── Sensor read functions ─────────────────────────────────────────────────────

bool readLiquidTemperature(float &tempC)
{
    if (!ds18b20Ready) { tempC = -1.0f; return false; }
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) { tempC = -1.0f; return false; }
    tempC = t;
    return true;
}

bool readPH(float &voltage, float &ph, float tempC = PH_CAL_REF_TEMP)
{
    uint32_t sum = 0;
    for (int i = 0; i < PH_OVERSAMPLE; i++) {
        sum += analogRead(PH_ADC_PIN);
        delayMicroseconds(200);
    }
    float raw = static_cast<float>(sum) / PH_OVERSAMPLE;
    voltage   = raw * 3.3f / 4095.0f;
    ph        = voltageToPH(voltage, tempC);
    return true;
}

bool readEnvironment(float &temperature, float &humidity, float &pressure)
{
    if (!bmeReady) { temperature = humidity = pressure = -1.0f; return false; }
    temperature = bme.readTemperature();
    humidity    = bme.readHumidity();
    pressure    = bme.readPressure() / 100.0f;
    return true;
}

bool isBMEReady()     { return bmeReady; }
bool isDS18B20Ready() { return ds18b20Ready; }

#endif // PH_TEMP_SENSORS_H
