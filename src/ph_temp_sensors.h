#ifndef PH_TEMP_SENSORS_H
#define PH_TEMP_SENSORS_H

#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>

// ── Pin configuration ────────────────────────────────────────────────────────
constexpr uint8_t PH_ADC_PIN   = 1;
constexpr uint8_t DS18B20_PIN  = 4;
constexpr uint8_t I2C_SDA_PIN  = 8;
constexpr uint8_t I2C_SCL_PIN  = 9;
constexpr uint8_t BMP280_ADDR  = 0x76;

// ── Calibration constants ─────────────────────────────────────────────────────
constexpr int   MAX_CAL_POINTS          = 5;
constexpr float CAL_PH_MATCH_WINDOW     = 0.2f;   // replace existing point if within this many pH units
constexpr float CAL_LINEARITY_WARN_PH   = 0.3f;   // residual threshold for non-linearity warning (pH units)
constexpr float PH_CAL_REF_TEMP         = 25.0f;  // temperature at which calibration is performed (°C)

// Theoretical defaults (Nernst: ~59 mV/pH at 25°C, zero-point near 2.5 V at pH 7)
constexpr float PH_DEFAULT_V_AT_7 = 2.50f;
constexpr float PH_DEFAULT_SLOPE  = -0.1667f; // V/pH  (= (3.00-2.50)/(4-7))

// ── ADC oversampling ─────────────────────────────────────────────────────────
constexpr int PH_OVERSAMPLE = 16;

// ── Calibration state ─────────────────────────────────────────────────────────

struct CalibrationPoint { float pH; float voltage; };

static CalibrationPoint g_calPoints[MAX_CAL_POINTS];
static int              g_calCount = 0;

// ── Calibration math (private helpers) ───────────────────────────────────────

// Least-squares linear regression: voltage = intercept + slope * pH
// Falls back to theoretical defaults when < 2 points are stored.
static void computeRegression(float &slope, float &intercept)
{
    if (g_calCount == 0) {
        slope     = PH_DEFAULT_SLOPE;
        intercept = PH_DEFAULT_V_AT_7 - slope * 7.0f;
        return;
    }
    if (g_calCount == 1) {
        slope     = PH_DEFAULT_SLOPE;
        intercept = g_calPoints[0].voltage - slope * g_calPoints[0].pH;
        return;
    }

    float n = (float)g_calCount;
    float sX = 0, sY = 0, sXY = 0, sX2 = 0;
    for (int i = 0; i < g_calCount; i++) {
        sX  += g_calPoints[i].pH;
        sY  += g_calPoints[i].voltage;
        sXY += g_calPoints[i].pH * g_calPoints[i].voltage;
        sX2 += g_calPoints[i].pH * g_calPoints[i].pH;
    }
    float denom = n * sX2 - sX * sX;
    if (fabsf(denom) < 1e-9f) {
        // Degenerate (all same pH) - use default slope
        slope     = PH_DEFAULT_SLOPE;
        intercept = sY / n - slope * sX / n;
        return;
    }
    slope     = (n * sXY - sX * sY) / denom;
    intercept = (sY - slope * sX) / n;
}

// Temperature-compensated voltage → pH conversion.
// Uses the isopotential-point approximation: the electrode voltage at pH 7
// is assumed invariant with temperature; only the slope scales with T.
static float voltageToPH(float v, float tempC = PH_CAL_REF_TEMP)
{
    float slope, intercept;
    computeRegression(slope, intercept);

    // Voltage at the isopotential point (pH 7) from the calibration line
    float V_iso   = intercept + slope * 7.0f;
    float tempFac = (tempC + 273.15f) / (PH_CAL_REF_TEMP + 273.15f);
    float slopeT  = slope * tempFac;

    if (fabsf(slopeT) < 1e-9f) return 7.0f;
    return constrain(7.0f + (v - V_iso) / slopeT, 0.0f, 14.0f);
}

// ── Calibration NVS helpers ───────────────────────────────────────────────────

static void sortCalibrationPoints()
{
    for (int i = 0; i < g_calCount - 1; i++)
        for (int j = i + 1; j < g_calCount; j++)
            if (g_calPoints[i].pH > g_calPoints[j].pH) {
                CalibrationPoint tmp = g_calPoints[i];
                g_calPoints[i]       = g_calPoints[j];
                g_calPoints[j]       = tmp;
            }
}

static void saveAllCalibration()
{
    Preferences prefs;
    prefs.begin("ph_cal", false);
    prefs.putInt("count", g_calCount);
    for (int i = 0; i < g_calCount; i++) {
        char kpH[8], kV[8];
        snprintf(kpH, sizeof(kpH), "pH%d", i);
        snprintf(kV,  sizeof(kV),  "V%d",  i);
        prefs.putFloat(kpH, g_calPoints[i].pH);
        prefs.putFloat(kV,  g_calPoints[i].voltage);
    }
    prefs.end();
}

void loadCalibration()
{
    Preferences prefs;
    prefs.begin("ph_cal", true);
    g_calCount = prefs.getInt("count", 0);
    if (g_calCount < 0 || g_calCount > MAX_CAL_POINTS) g_calCount = 0;
    for (int i = 0; i < g_calCount; i++) {
        char kpH[8], kV[8];
        snprintf(kpH, sizeof(kpH), "pH%d", i);
        snprintf(kV,  sizeof(kV),  "V%d",  i);
        g_calPoints[i].pH      = prefs.getFloat(kpH, 7.0f);
        g_calPoints[i].voltage = prefs.getFloat(kV,  PH_DEFAULT_V_AT_7);
    }
    prefs.end();

    Serial.printf("[CAL] Loaded %d calibration point(s)\n", g_calCount);
    for (int i = 0; i < g_calCount; i++)
        Serial.printf("[CAL]   pH %.2f -> %.4f V\n", g_calPoints[i].pH, g_calPoints[i].voltage);
}

void resetCalibration()
{
    g_calCount = 0;
    Preferences prefs;
    prefs.begin("ph_cal", false);
    prefs.clear();
    prefs.end();
    Serial.println("[CAL] Reset — using theoretical defaults");
}

// ── Public calibration API ────────────────────────────────────────────────────

// Returns maximum residual across all stored points in pH units.
// 0.0 when fewer than 3 points are stored (2-point fit is always exact).
float getCalibrationLinearity()
{
    if (g_calCount < 3) return 0.0f;
    float slope, intercept;
    computeRegression(slope, intercept);
    if (fabsf(slope) < 1e-9f) return 0.0f;

    float maxPH = 0.0f;
    for (int i = 0; i < g_calCount; i++) {
        float predV    = intercept + slope * g_calPoints[i].pH;
        float residPH  = fabsf(g_calPoints[i].voltage - predV) / fabsf(slope);
        if (residPH > maxPH) maxPH = residPH;
    }
    return maxPH;
}

// Adds or updates a calibration point, then saves to NVS.
// • Replaces the nearest existing point if within CAL_PH_MATCH_WINDOW.
// • Adds a new point if capacity allows.
// • At MAX_CAL_POINTS, replaces the nearest point regardless.
// Returns the max linearity residual in pH units after the update.
float addCalibrationPoint(float bufferPH, float voltage)
{
    int   closestIdx  = -1;
    float closestDist = 1e9f;
    for (int i = 0; i < g_calCount; i++) {
        float d = fabsf(g_calPoints[i].pH - bufferPH);
        if (d < closestDist) { closestDist = d; closestIdx = i; }
    }

    if (closestIdx >= 0 && closestDist <= CAL_PH_MATCH_WINDOW) {
        g_calPoints[closestIdx] = {bufferPH, voltage};
    } else if (g_calCount < MAX_CAL_POINTS) {
        g_calPoints[g_calCount++] = {bufferPH, voltage};
    } else {
        g_calPoints[closestIdx] = {bufferPH, voltage}; // at capacity: replace nearest
    }

    sortCalibrationPoints();
    saveAllCalibration();

    Serial.printf("[CAL] Added pH %.2f -> %.4f V  (%d point(s) total)\n",
                  bufferPH, voltage, g_calCount);
    return getCalibrationLinearity();
}

void getCalibrationRegression(float &slope, float &intercept) { computeRegression(slope, intercept); }
int  getCalibrationCount()                                     { return g_calCount; }
void getCalibrationPoints(const CalibrationPoint **out, int &count) { *out = g_calPoints; count = g_calCount; }

// ── Sensor init ───────────────────────────────────────────────────────────────

static OneWire           oneWire(DS18B20_PIN);
static DallasTemperature ds18b20(&oneWire);
static bool              ds18b20Ready = false;

static Adafruit_BMP280 bmp;
static bool            bmpReady = false;

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
    bmpReady = bmp.begin(BMP280_ADDR);
    if (bmpReady) {
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                        Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16,
                        Adafruit_BMP280::FILTER_X16,
                        Adafruit_BMP280::STANDBY_MS_500);
        Serial.printf("[SENSOR] BMP280 OK  addr=0x%02X\n", BMP280_ADDR);
    } else {
        Serial.printf("[SENSOR] BMP280 NOT found at 0x%02X\n", BMP280_ADDR);
    }

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

bool readEnvironment(float &temperature, float &pressure)
{
    if (!bmpReady) { temperature = pressure = -1.0f; return false; }
    temperature = bmp.readTemperature();
    pressure    = bmp.readPressure() / 100.0f;
    return true;
}

bool isBMPReady()     { return bmpReady; }
bool isDS18B20Ready() { return ds18b20Ready; }

#endif // PH_TEMP_SENSORS_H
