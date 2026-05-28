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
                               "Calibrate the pH sensor at a single buffer point. "
                               "Place the probe in the buffer solution and wait for the reading "
                               "to stabilize, then call this function.",
                               16, 256)
        .argFloat("buffer_ph", "Buffer pH", "Known pH of the calibration buffer — must be 4.0 or 7.0")
        .returnFloat("measured_voltage", "Measured Voltage", "ADC voltage recorded at this calibration point (V)")
        .returnFloat("buffer_ph",        "Buffer pH",        "The pH value the calibration was recorded for")
        .returnFloat("v_at_7",           "V at pH 7",        "Full stored calibration: voltage at pH 7 (V)")
        .returnFloat("v_at_4",           "V at pH 4",        "Full stored calibration: voltage at pH 4 (V)")
        .build();

    app.registerFunction("calibrate_ph_point", def,
        [](ArkitektApp &, Agent &, JsonObject args, ReplyChannel &reply) -> bool {
            float bufferPH = args["buffer_ph"] | 0.0f;
            if (fabsf(bufferPH - 7.0f) > 0.01f && fabsf(bufferPH - 4.0f) > 0.01f) {
                reply.critical("buffer_ph must be 4.0 or 7.0");
                return false;
            }

            float voltage, ph, liquidTemp;
            bool ds18b20Ok = readLiquidTemperature(liquidTemp);
            readPH(voltage, ph, ds18b20Ok ? liquidTemp : PH_CAL_REF_TEMP);

            if (!saveCalibrationPoint(bufferPH, voltage)) {
                reply.critical("Failed to save calibration point to NVS");
                return false;
            }

            float v7, v4;
            getCalibrationVoltages(v7, v4);

            StaticJsonDocument<256> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["measured_voltage"] = voltage;
            ret["buffer_ph"]        = bufferPH;
            ret["v_at_7"]           = v7;
            ret["v_at_4"]           = v4;
            reply.done(ret);
            return true;
        });
}

void registerGetCalibration()
{
    auto def = FunctionBuilder("get_calibration",
                               "Returns the current pH calibration values stored on the device.",
                               16, 128)
        .returnFloat("v_at_7", "V at pH 7", "Stored calibration voltage at pH 7 (V)")
        .returnFloat("v_at_4", "V at pH 4", "Stored calibration voltage at pH 4 (V)")
        .build();

    app.registerFunction("get_calibration", def,
        [](ArkitektApp &, Agent &, JsonObject, ReplyChannel &reply) -> bool {
            float v7, v4;
            getCalibrationVoltages(v7, v4);
            StaticJsonDocument<128> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["v_at_7"] = v7;
            ret["v_at_4"] = v4;
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
        .returnFloat("temperature",        "Ambient Temp", "BME280 ambient temperature (C); -1 if absent")
        .returnFloat("humidity",           "Humidity",     "BME280 humidity (%RH); -1 if absent")
        .returnFloat("pressure",           "Pressure",     "BME280 pressure (hPa); -1 if absent")
        .returnBool("bme_ok",              "BME280 OK",    "False if BME280 did not respond")
        .returnBool("ds18b20_ok",          "DS18B20 OK",   "False if DS18B20 did not respond")
        .build();

    app.registerFunction("read_sensors", def,
        [](ArkitektApp &, Agent &, JsonObject, ReplyChannel &reply) -> bool {
            float voltage, ph, liquidTemp, temp, humidity, pressure;
            bool ds18b20Ok = readLiquidTemperature(liquidTemp);
            readPH(voltage, ph, ds18b20Ok ? liquidTemp : PH_CAL_REF_TEMP);
            bool bmeOk = readEnvironment(temp, humidity, pressure);

            StaticJsonDocument<384> doc;
            JsonObject ret = doc.to<JsonObject>();
            ret["ph"]                 = ph;
            ret["ph_voltage"]         = voltage;
            ret["liquid_temperature"] = liquidTemp;
            ret["temperature"]        = temp;
            ret["humidity"]           = humidity;
            ret["pressure"]           = pressure;
            ret["bme_ok"]             = bmeOk;
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
        auto def = StateBuilder("ph_status", "pH Sensor", 384)
            .portFloat("ph",                 "pH",          "Temperature-compensated pH (0-14)")
            .portFloat("voltage",            "Voltage",     "Raw ADC voltage (V)")
            .portFloat("liquid_temperature", "Liquid Temp", "DS18B20 solution temperature (C)")
            .portBool("ds18b20_ok",          "DS18B20 OK",  "False if sensor absent")
            .portInt("readings_count",       "Readings",    "Total readings since boot")
            .build();

        app.registerState("ph_status", def, [](AgentState *state) {
            state->setPort("ph",                 7.0f);
            state->setPort("voltage",            0.0f);
            state->setPort("liquid_temperature", 25.0f);
            state->setPort("ds18b20_ok",         false);
            state->setPort("readings_count",     0);
        });
    }
    {
        auto def = StateBuilder("environment_status", "Environment", 384)
            .portFloat("temperature",  "Temperature", "BME280 ambient temperature (C)")
            .portFloat("humidity",     "Humidity",    "%RH")
            .portFloat("pressure",     "Pressure",    "hPa")
            .portBool("bme_ok",        "BME280 OK",   "False if sensor absent")
            .portInt("readings_count", "Readings",    "Total readings since boot")
            .build();

        app.registerState("environment_status", def, [](AgentState *state) {
            state->setPort("temperature",    0.0f);
            state->setPort("humidity",       0.0f);
            state->setPort("pressure",       0.0f);
            state->setPort("bme_ok",         false);
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

            float voltage, ph, liquidTemp, temp, humidity, pressure;
            bool ds18b20Ok = readLiquidTemperature(liquidTemp);
            readPH(voltage, ph, ds18b20Ok ? liquidTemp : PH_CAL_REF_TEMP);
            bool bmeOk = readEnvironment(temp, humidity, pressure);

            AgentState *phState = agent.getState("ph_status");
            if (phState) {
                phState->setPort("ph",                 ph);
                phState->setPort("voltage",            voltage);
                phState->setPort("liquid_temperature", ds18b20Ok ? liquidTemp : -1.0f);
                phState->setPort("ds18b20_ok",         ds18b20Ok);
                phState->setPort("readings_count",     count);
            }

            AgentState *envState = agent.getState("environment_status");
            if (envState) {
                envState->setPort("temperature",    bmeOk ? temp     : -1.0f);
                envState->setPort("humidity",       bmeOk ? humidity : -1.0f);
                envState->setPort("pressure",       bmeOk ? pressure : -1.0f);
                envState->setPort("bme_ok",         bmeOk);
                envState->setPort("readings_count", count);
            }

            Serial.printf("[BG] #%d | pH %.2f (%.3fV) | liquid %.1fC (%s) | ambient %.1fC %.1f%%RH %.1fhPa (%s)\n",
                          count, ph, voltage,
                          ds18b20Ok ? liquidTemp : -1.0f, ds18b20Ok ? "OK" : "MISSING",
                          bmeOk ? temp : -1.0f, bmeOk ? humidity : -1.0f, bmeOk ? pressure : -1.0f,
                          bmeOk ? "OK" : "MISSING");
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
