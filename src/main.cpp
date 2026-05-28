#include <Arduino.h>
#include "lib/arkitekt_app.h"
#include "ph_temp_sensors.h"

// ── Board constants ───────────────────────────────────────────────────────────
#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr uint8_t LED_PIN = 21;
#else
constexpr uint8_t LED_PIN = 2;
#endif
constexpr uint32_t SENSOR_INTERVAL_MS = 5000;

// ── App (global, matches upstream style) ─────────────────────────────────────
ArkitektApp app("ph-monitor", "1.0.0", "default", "pH & Temperature Monitor");

// ── Functions ─────────────────────────────────────────────────────────────────

void registerToggleLed()
{
    auto def = FunctionBuilder("toggle_led", "Toggles the built-in LED", 16, 128)
        .returnInt("pin",    "Pin",   "GPIO pin that was toggled")
        .returnBool("state", "State", "New LED state (true = on)")
        .build();

    app.registerFunction("toggle_led", def,
        [](ArkitektApp &, Agent &agent, JsonObject, ReplyChannel &reply) -> bool {
            static bool on = true;
            on = !on;
            digitalWrite(LED_PIN, on ? HIGH : LOW);

            StaticJsonDocument<64> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["pin"]   = LED_PIN;
            ret["state"] = on;
            reply.done(ret);

            AgentState *st = agent.getState("led_status");
            if (st) st->setPort("on", on);

            return true;
        });
}

void registerCalibratePhPoint()
{
    auto def = FunctionBuilder("calibrate_ph_point",
                               "Record a calibration point at any known buffer pH. "
                               "Place the probe in the buffer and wait for the reading to stabilize, "
                               "then call this function. Replaces the nearest stored point if within "
                               "±0.2 pH, otherwise adds a new one (up to 5 points). "
                               "With 3 or more points, electrode linearity is automatically checked.",
                               16, 512)
        .argFloat("buffer_ph", "Buffer pH", "Known pH of the calibration buffer (e.g. 4.01, 6.86, 9.18)")
        .returnFloat("measured_voltage", "Measured Voltage",   "ADC voltage recorded at this buffer (V)")
        .returnFloat("buffer_ph",        "Buffer pH",          "pH value the point was recorded at")
        .returnInt("cal_count",          "Calibration Points", "Total stored calibration points after this update")
        .returnFloat("slope",            "Slope (V/pH)",       "Regression slope — typically −0.05 to −0.07 V/pH for a good electrode")
        .returnFloat("intercept",        "Intercept (V)",      "Regression intercept (voltage extrapolated to pH 0)")
        .returnBool("linearity_ok",      "Linearity OK",       "True if all points fit the regression line within tolerance")
        .returnFloat("max_residual_ph",  "Max Residual (pH)",  "Largest deviation of any stored point from the regression line")
        .build();

    app.registerFunction("calibrate_ph_point", def,
        [](ArkitektApp &, Agent &, JsonObject args, ReplyChannel &reply) -> bool {
            float bufferPH = args["buffer_ph"] | -1.0f;
            if (bufferPH < 0.0f || bufferPH > 14.0f) {
                reply.critical("buffer_ph must be between 0 and 14");
                return false;
            }

            float voltage, ph, liquidTemp;
            bool ds18b20Ok = readLiquidTemperature(liquidTemp);
            readPH(voltage, ph, ds18b20Ok ? liquidTemp : PH_CAL_REF_TEMP);

            float maxResidualPH = addCalibrationPoint(bufferPH, voltage);

            float slope, intercept;
            getCalibrationRegression(slope, intercept);
            int   calCount    = getCalibrationCount();
            bool  linearityOk = (calCount < 3) || (maxResidualPH <= CAL_LINEARITY_WARN_PH);

            if (!linearityOk) {
                String warn = "Calibration point at pH ";
                warn += String(bufferPH, 2);
                warn += " deviates ";
                warn += String(maxResidualPH, 2);
                warn += " pH units from the regression line. "
                        "This suggests the electrode response is not linear — "
                        "consider replacing the probe.";
                reply.log(warn, "WARNING");
            }

            StaticJsonDocument<384> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["measured_voltage"] = voltage;
            ret["buffer_ph"]        = bufferPH;
            ret["cal_count"]        = calCount;
            ret["slope"]            = slope;
            ret["intercept"]        = intercept;
            ret["linearity_ok"]     = linearityOk;
            ret["max_residual_ph"]  = maxResidualPH;
            reply.done(ret);
            return true;
        });
}

void registerGetCalibration()
{
    auto def = FunctionBuilder("get_calibration",
                               "Returns current calibration: regression parameters, all stored buffer points, "
                               "and a linearity check. Up to 5 points are shown as point_N_ph / point_N_v.",
                               16, 512)
        .returnInt("cal_count",         "Calibration Points", "Number of stored calibration points (0 = using theoretical defaults)")
        .returnFloat("slope",           "Slope (V/pH)",       "Regression slope")
        .returnFloat("intercept",       "Intercept (V)",      "Regression intercept")
        .returnBool("linearity_ok",     "Linearity OK",       "True if all points fit the line within tolerance")
        .returnFloat("max_residual_ph", "Max Residual (pH)",  "Largest per-point deviation from the regression line")
        .returnFloat("point_0_ph",  "Point 0 pH",  "Calibration point 0 pH  (−1 if unused)").returnFloat("point_0_v", "Point 0 V", "")
        .returnFloat("point_1_ph",  "Point 1 pH",  "Calibration point 1 pH  (−1 if unused)").returnFloat("point_1_v", "Point 1 V", "")
        .returnFloat("point_2_ph",  "Point 2 pH",  "Calibration point 2 pH  (−1 if unused)").returnFloat("point_2_v", "Point 2 V", "")
        .returnFloat("point_3_ph",  "Point 3 pH",  "Calibration point 3 pH  (−1 if unused)").returnFloat("point_3_v", "Point 3 V", "")
        .returnFloat("point_4_ph",  "Point 4 pH",  "Calibration point 4 pH  (−1 if unused)").returnFloat("point_4_v", "Point 4 V", "")
        .build();

    app.registerFunction("get_calibration", def,
        [](ArkitektApp &, Agent &, JsonObject, ReplyChannel &reply) -> bool {
            float slope, intercept;
            getCalibrationRegression(slope, intercept);
            int   calCount      = getCalibrationCount();
            float maxResidualPH = getCalibrationLinearity();
            bool  linearityOk   = (calCount < 3) || (maxResidualPH <= CAL_LINEARITY_WARN_PH);

            const CalibrationPoint *pts;
            int cnt;
            getCalibrationPoints(&pts, cnt);

            StaticJsonDocument<512> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["cal_count"]        = calCount;
            ret["slope"]            = slope;
            ret["intercept"]        = intercept;
            ret["linearity_ok"]     = linearityOk;
            ret["max_residual_ph"]  = maxResidualPH;
            for (int i = 0; i < MAX_CAL_POINTS; i++) {
                String kph = "point_" + String(i) + "_ph";
                String kv  = "point_" + String(i) + "_v";
                ret[kph]   = (i < cnt) ? pts[i].pH      : -1.0f;
                ret[kv]    = (i < cnt) ? pts[i].voltage  : -1.0f;
            }
            reply.done(ret);
            return true;
        });
}

void registerResetCalibration()
{
    auto def = FunctionBuilder("reset_calibration",
                               "Resets pH calibration to factory defaults and clears NVS storage.",
                               16, 64)
        .returnBool("success", "Success", "True if reset succeeded")
        .build();

    app.registerFunction("reset_calibration", def,
        [](ArkitektApp &, Agent &, JsonObject, ReplyChannel &reply) -> bool {
            resetCalibration();
            StaticJsonDocument<64> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["success"] = true;
            reply.done(ret);
            return true;
        });
}

void registerReadSensors()
{
    auto def = FunctionBuilder("read_sensors",
                               "Triggers an immediate reading from all sensors.",
                               16, 512)
        .returnFloat("ph",                 "pH",           "Temperature-compensated pH (0-14)")
        .returnFloat("ph_voltage",         "pH Voltage",   "Raw ADC voltage (V)")
        .returnFloat("liquid_temperature", "Liquid Temp",  "DS18B20 solution temperature (C); -1 if absent")
        .returnFloat("temperature",        "Ambient Temp", "BMP280 ambient temperature (C); -1 if absent")
        .returnFloat("pressure",           "Pressure",     "BMP280 pressure (hPa); -1 if absent")
        .returnBool("bmp_ok",              "BMP280 OK",    "False if BMP280 did not respond")
        .returnBool("ds18b20_ok",          "DS18B20 OK",   "False if DS18B20 did not respond")
        .build();

    app.registerFunction("read_sensors", def,
        [](ArkitektApp &, Agent &, JsonObject, ReplyChannel &reply) -> bool {
            float voltage, ph, liquidTemp, temp, pressure;
            bool ds18b20Ok = readLiquidTemperature(liquidTemp);
            readPH(voltage, ph, ds18b20Ok ? liquidTemp : PH_CAL_REF_TEMP);
            bool bmpOk = readEnvironment(temp, pressure);

            StaticJsonDocument<256> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["ph"]                 = ph;
            ret["ph_voltage"]         = voltage;
            ret["liquid_temperature"] = liquidTemp;
            ret["temperature"]        = temp;
            ret["pressure"]           = pressure;
            ret["bmp_ok"]             = bmpOk;
            ret["ds18b20_ok"]         = ds18b20Ok;
            reply.done(ret);
            return true;
        });
}

// ── States ────────────────────────────────────────────────────────────────────

void registerStates()
{
    {
        auto def = StateBuilder("led_status", "LED Status", 128)
            .portBool("on",  "On",  "LED on/off")
            .portInt("pin",  "Pin", "GPIO pin")
            .build();

        app.registerState("led_status", def, [](AgentState *state) {
            state->setPort("on",  true);
            state->setPort("pin", (int)LED_PIN);
        });
    }
    {
        auto def = StateBuilder("ph_status", "pH Sensor", 512)
            .portFloat("ph",                 "pH",            "Temperature-compensated pH (0-14)")
            .portFloat("voltage",            "Voltage",       "Raw ADC voltage (V)")
            .portFloat("liquid_temperature", "Liquid Temp",   "DS18B20 solution temperature (C)")
            .portBool("ds18b20_ok",          "DS18B20 OK",    "False if sensor absent")
            .portInt("cal_count",            "Cal Points",    "Number of stored calibration points (0 = theoretical defaults)")
            .portFloat("cal_slope",          "Cal Slope",     "Calibration regression slope (V/pH)")
            .portBool("cal_linearity_ok",    "Cal Linear",    "False if any calibration point deviates beyond tolerance")
            .portInt("readings_count",       "Readings",      "Total readings since boot")
            .build();

        app.registerState("ph_status", def, [](AgentState *state) {
            float slope, intercept;
            getCalibrationRegression(slope, intercept);
            state->setPort("ph",                 7.0f);
            state->setPort("voltage",            0.0f);
            state->setPort("liquid_temperature", 25.0f);
            state->setPort("ds18b20_ok",         false);
            state->setPort("cal_count",          getCalibrationCount());
            state->setPort("cal_slope",          slope);
            state->setPort("cal_linearity_ok",   true);
            state->setPort("readings_count",     0);
        });
    }
    {
        auto def = StateBuilder("environment_status", "Environment", 256)
            .portFloat("temperature",  "Temperature", "BMP280 ambient temperature (C)")
            .portFloat("pressure",     "Pressure",    "hPa")
            .portBool("bmp_ok",        "BMP280 OK",   "False if sensor absent")
            .portInt("readings_count", "Readings",    "Total readings since boot")
            .build();

        app.registerState("environment_status", def, [](AgentState *state) {
            state->setPort("temperature",    0.0f);
            state->setPort("pressure",       0.0f);
            state->setPort("bmp_ok",         false);
            state->setPort("readings_count", 0);
        });
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 3000));

    Serial.println("\n=== pH Monitor booting ===");
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    initSensors();
    loadCalibration();

    app.addRequirement("rekuest", "live.arkitekt.rekuest");

    registerToggleLed();
    registerCalibratePhPoint();
    registerGetCalibration();
    registerResetCalibration();
    registerReadSensors();
    registerStates();

    app.registerBackgroundTask(
        [](ArkitektApp &, Agent &agent) {
            static int count = 0;
            count++;

            float voltage, ph, liquidTemp, temp, pressure;
            bool ds18b20Ok = readLiquidTemperature(liquidTemp);
            readPH(voltage, ph, ds18b20Ok ? liquidTemp : PH_CAL_REF_TEMP);
            bool bmpOk = readEnvironment(temp, pressure);

            AgentState *phState = agent.getState("ph_status");
            if (phState) {
                float slope, intercept;
                getCalibrationRegression(slope, intercept);
                int  calCount    = getCalibrationCount();
                bool linearityOk = (calCount < 3) || (getCalibrationLinearity() <= CAL_LINEARITY_WARN_PH);
                phState->setPort("ph",                 ph);
                phState->setPort("voltage",            voltage);
                phState->setPort("liquid_temperature", ds18b20Ok ? liquidTemp : -1.0f);
                phState->setPort("ds18b20_ok",         ds18b20Ok);
                phState->setPort("cal_count",          calCount);
                phState->setPort("cal_slope",          slope);
                phState->setPort("cal_linearity_ok",   linearityOk);
                phState->setPort("readings_count",     count);
            }

            AgentState *envState = agent.getState("environment_status");
            if (envState) {
                envState->setPort("temperature",    bmpOk ? temp     : -1.0f);
                envState->setPort("pressure",       bmpOk ? pressure : -1.0f);
                envState->setPort("bmp_ok",         bmpOk);
                envState->setPort("readings_count", count);
            }

            Serial.printf("[BG] #%d | pH %.2f (%.3fV) | liquid %.1fC (%s) | ambient %.1fC %.1fhPa (%s)\n",
                          count, ph, voltage,
                          ds18b20Ok ? liquidTemp : -1.0f, ds18b20Ok ? "OK" : "MISSING",
                          bmpOk ? temp : -1.0f, bmpOk ? pressure : -1.0f,
                          bmpOk ? "OK" : "MISSING");
        },
        SENSOR_INTERVAL_MS);

    RunConfig cfg;
    cfg.ble                    = true;
    cfg.enableWpa2Enterprise   = true;
    cfg.bootReconfigureTimeout = 5000;
    app.run(cfg);
}

void loop()
{
    app.loop();
}
