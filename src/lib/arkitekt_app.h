#ifndef ARKITEKT_APP_H
#define ARKITEKT_APP_H

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wpa2.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <functional>
#include <vector>
#include "mbedtls/sha256.h"

// TODO: Only for esp32s3
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "esp_efuse.h"
#endif 

#include "config_defaults.h"
#include "manifest.h"
#include "fakts.h"
#include "auth.h"
#include "app.h"
#include "agent.h"
#include "port_builder.h"
#include "function_builder.h"
#include "state_builder.h"
#include "reply_channel.h"
#include "run_config.h"

// BLE UUIDs for provisioning service
#define ARKITEKT_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define ARKITEKT_WIFI_SSID_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define ARKITEKT_WIFI_PASSWORD_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define ARKITEKT_BASE_URL_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define ARKITEKT_FAKTS_TOKEN_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define ARKITEKT_WIFI_IDENTITY_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ae"
#define ARKITEKT_WIFI_ANON_IDENTITY_UUID "beb5483e-36e1-4688-b7f5-ea07361b26af"
#define ARKITEKT_WIFI_PEM_CERT_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b0"
#define ARKITEKT_MANIFEST_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ad"
#define ARKITEKT_STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"

// Background task definition
struct BackgroundTask
{
    std::function<void(ArkitektApp &, Agent &)> callback;
    unsigned long intervalMs;
    unsigned long lastRun;

    BackgroundTask(std::function<void(ArkitektApp &, Agent &)> cb, unsigned long interval)
        : callback(cb), intervalMs(interval), lastRun(0) {}
};

// Singleton pointer for BLE callbacks (BLE callbacks can't capture class context)
class ArkitektApp;
static ArkitektApp *_arkitektAppInstance = nullptr;

class ArkitektApp
{
private:
    // Identity
    String instanceId;
    String agentName;

    // Core objects
    Manifest manifest;
    FaktsConfig faktsConfig;
    App app;
    Agent *agent;
    WebSocketsClient webSocket;
    Preferences preferences;

    // Config
    RunConfig config;
    char baseUrl[128];
    char faktsToken[128];
    String claimUrl;
    String accessToken;
    bool appReady;

    // BLE state
    BLEServer *pServer;
    BLECharacteristic *statusCharacteristic;
    bool deviceConnected;
    bool oldDeviceConnected;
    String bleWifiSSID;
    String bleWifiPassword;
    String bleWifiIdentity;
    String bleWifiAnonIdentity;
    String bleWifiPemCertificate;
    String bleConfigBaseUrl;
    String bleConfigToken;
    bool hasNewConfig;

    // Factory reset
    unsigned long bootButtonPressStart;
    bool bootButtonHeld;

    // Background tasks
    std::vector<BackgroundTask> backgroundTasks;

    // ======================== BLE Callbacks ========================

    class ServerCallbacks : public BLEServerCallbacks
    {
        void onConnect(BLEServer *pServer) override
        {
            if (_arkitektAppInstance)
                _arkitektAppInstance->deviceConnected = true;
            Serial.println("[BLE] Client connected");
        }
        void onDisconnect(BLEServer *pServer) override
        {
            if (_arkitektAppInstance)
                _arkitektAppInstance->deviceConnected = false;
            Serial.println("[BLE] Client disconnected");
        }
    };

    class SSIDCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    _arkitektAppInstance->bleWifiSSID = String(v.c_str());
                    Serial.println("[BLE] SSID: " + _arkitektAppInstance->bleWifiSSID);
                }
            }
        }
    };

    class PasswordCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    _arkitektAppInstance->bleWifiPassword = String(v.c_str());
                    Serial.println("[BLE] Password: " + String(v.length()) + " chars");
                }
            }
        }
    };

    class IdentityCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    _arkitektAppInstance->bleWifiIdentity = String(v.c_str());
                    Serial.println("[BLE] Identity: " + _arkitektAppInstance->bleWifiIdentity);
                }
            }
        }
    };

    class AnonIdentityCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    _arkitektAppInstance->bleWifiAnonIdentity = String(v.c_str());
                    Serial.println("[BLE] AnonIdentity: " + _arkitektAppInstance->bleWifiAnonIdentity);
                }
            }
        }
    };

    class PemCertCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    if (v == "CLEAR")
                    {
                        _arkitektAppInstance->bleWifiPemCertificate = "";
                        Serial.println("[BLE] PEM buffer cleared");
                    }
                    else
                    {
                        _arkitektAppInstance->bleWifiPemCertificate += String(v.c_str());
                        Serial.println("[BLE] PEM chunk: " + String(v.length()) + " bytes, total: " + String(_arkitektAppInstance->bleWifiPemCertificate.length()));
                    }
                }
            }
        }
    };

    class BaseURLCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    _arkitektAppInstance->bleConfigBaseUrl = String(v.c_str());
                    Serial.println("[BLE] Base URL: " + _arkitektAppInstance->bleConfigBaseUrl);
                }
            }
        }
    };

    class TokenCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCh) override
        {
            if (_arkitektAppInstance)
            {
                String v = String(pCh->getValue().c_str());
                if (v.length() > 0)
                {
                    _arkitektAppInstance->bleConfigToken = String(v.c_str());
                    _arkitektAppInstance->hasNewConfig = true;
                    Serial.println("[BLE] Token received, config complete");
                }
            }
        }
    };

    // ======================== Device ID Generation ========================

    void generateDeviceId()
    {
        uint8_t mac[6];
        // if ESP32, we use a UUID v5-like approach based on the MAC address, chip revision, and model to generate a unique but consistent device ID that doesn't directly expose the MAC
        #ifdef CONFIG_IDF_TARGET_ESP32
        // add some value to the MAC to obfuscate it, since ESP32's MAC is easily obtainable by other apps and we don't want to make it trivially easy to correlate the device ID with the MAC
        // (especially since the MAC is used in the BLE advertising, which can be easily snifed by nearby devices)
        
        #else 
        esp_efuse_mac_get_default(mac); 
        #endif
        const char *ns = "arkitekt-esp32-device-id";
        uint8_t chipRev = ESP.getChipRevision();
        uint32_t chipModel = ESP.getChipModel()[0];

        uint8_t input[64];
        size_t len = 0;
        memcpy(input + len, ns, strlen(ns));
        len += strlen(ns);
        memcpy(input + len, mac, 6);
        len += 6;
        input[len++] = chipRev;
        input[len++] = (uint8_t)(chipModel & 0xFF);

        uint8_t hash[32];
        mbedtls_sha256(input, len, hash, 0);

        // UUID v5-style
        hash[6] = (hash[6] & 0x0F) | 0x50;
        hash[8] = (hash[8] & 0x3F) | 0x80;

        char deviceId[37];
        snprintf(deviceId, sizeof(deviceId),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 hash[0], hash[1], hash[2], hash[3],
                 hash[4], hash[5], hash[6], hash[7],
                 hash[8], hash[9], hash[10], hash[11],
                 hash[12], hash[13], hash[14], hash[15]);

        manifest.addDeviceId(deviceId);
        Serial.println("Device ID: " + String(deviceId));
        Serial.printf("  (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // ======================== Agent Name Generation ========================

    void generateAgentName()
    {
        // Adjective + noun picked deterministically from the MAC address,
        // followed by the last 4 hex digits of the MAC for uniqueness.
        static const char *adjectives[] = {
            "lucky", "precise", "quick", "bright", "calm",
            "eager", "fancy", "happy", "idle", "jolly",
            "keen", "lively", "merry", "neat", "odd",
            "peppy", "quiet", "rapid", "swift", "tidy",
            "alert", "bold", "crisp", "deft", "epic"
        };
        static const char *nouns[] = {
            "microscope", "lens", "sensor", "probe", "scope",
            "meter", "module", "beacon", "node", "chip",
            "board", "device", "relay", "servo", "motor",
            "driver", "bridge", "gate", "hub", "laser",
            "fiber", "beam", "filter", "stage", "focus"
        };

        uint8_t mac[6];
        #ifdef CONFIG_IDF_TARGET_ESP32
        // for ESP32, use the same obfuscated MAC as in device ID generation to ensure the agent name is also not trivially linkable to the real MAC
        #else
        esp_efuse_mac_get_default(mac);
        #endif
        const int adjCount = sizeof(adjectives) / sizeof(adjectives[0]);
        const int nounCount = sizeof(nouns) / sizeof(nouns[0]);

        // XOR alternating bytes so adjacent MACs still spread across the table
        int adjIdx  = (mac[0] ^ mac[2] ^ mac[4]) % adjCount;
        int nounIdx = (mac[1] ^ mac[3] ^ mac[5]) % nounCount;

        char suffix[5];
        snprintf(suffix, sizeof(suffix), "%02x%02x", mac[4], mac[5]);

        agentName = String("esp32-") + adjectives[adjIdx] + "-" + nouns[nounIdx] + "-" + suffix;
        Serial.println("Agent name: " + agentName);
    }

    // ======================== BLE Provisioning ========================

    void startBLEProvisioning()
    {
        Serial.println("\n=== Starting BLE Provisioning ===");

        BLEDevice::init(config.bleDeviceName);
        BLEDevice::setMTU(517);

        pServer = BLEDevice::createServer();
        pServer->setCallbacks(new ServerCallbacks());

        BLEService *pService = pServer->createService(BLEUUID(ARKITEKT_SERVICE_UUID), 30);

        auto *ssidChar = pService->createCharacteristic(ARKITEKT_WIFI_SSID_UUID, BLECharacteristic::PROPERTY_WRITE);
        ssidChar->setCallbacks(new SSIDCallback());

        auto *passChar = pService->createCharacteristic(ARKITEKT_WIFI_PASSWORD_UUID, BLECharacteristic::PROPERTY_WRITE);
        passChar->setCallbacks(new PasswordCallback());

        auto *idChar = pService->createCharacteristic(ARKITEKT_WIFI_IDENTITY_UUID, BLECharacteristic::PROPERTY_WRITE);
        idChar->setCallbacks(new IdentityCallback());

        auto *anonIdChar = pService->createCharacteristic(ARKITEKT_WIFI_ANON_IDENTITY_UUID, BLECharacteristic::PROPERTY_WRITE);
        anonIdChar->setCallbacks(new AnonIdentityCallback());

        auto *pemChar = pService->createCharacteristic(ARKITEKT_WIFI_PEM_CERT_UUID, BLECharacteristic::PROPERTY_WRITE);
        pemChar->setCallbacks(new PemCertCallback());

        auto *urlChar = pService->createCharacteristic(ARKITEKT_BASE_URL_UUID, BLECharacteristic::PROPERTY_WRITE);
        urlChar->setCallbacks(new BaseURLCallback());

        auto *tokenChar = pService->createCharacteristic(ARKITEKT_FAKTS_TOKEN_UUID, BLECharacteristic::PROPERTY_WRITE);
        tokenChar->setCallbacks(new TokenCallback());

        // Manifest (read-only)
        auto *manifestChar = pService->createCharacteristic(ARKITEKT_MANIFEST_UUID, BLECharacteristic::PROPERTY_READ);
        JsonDocument manifestDoc;
        JsonObject manifestObj = manifestDoc.to<JsonObject>();
        manifest.toJson(manifestObj);
        String manifestJson;
        serializeJson(manifestDoc, manifestJson);
        manifestChar->setValue(manifestJson.c_str());

        // Status (read + notify)
        statusCharacteristic = pService->createCharacteristic(
            ARKITEKT_STATUS_UUID,
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
        statusCharacteristic->addDescriptor(new BLE2902());
        statusCharacteristic->setValue("Ready");

        pService->start();

        BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
        pAdvertising->addServiceUUID(ARKITEKT_SERVICE_UUID);
        pAdvertising->setScanResponse(true);
        pAdvertising->setMinPreferred(0x06);
        pAdvertising->setMinPreferred(0x12);
        BLEDevice::startAdvertising();

        Serial.println("BLE advertising as: " + String(config.bleDeviceName));
        Serial.println("Waiting for BLE connection...");
    }

    void waitForBLEConfig()
    {
        while (!hasNewConfig)
        {
            delay(100);
            if (!deviceConnected && oldDeviceConnected)
            {
                delay(500);
                pServer->startAdvertising();
                oldDeviceConnected = deviceConnected;
            }
            if (deviceConnected && !oldDeviceConnected)
            {
                oldDeviceConnected = deviceConnected;
            }
        }

        Serial.println("\n=== Configuration Received ===");

        if (statusCharacteristic)
        {
            statusCharacteristic->setValue("Connecting...");
            statusCharacteristic->notify();
        }

        // Save config to NVS
        preferences.begin("arkitekt", false);
        if (bleConfigBaseUrl.length() > 0)
        {
            preferences.putString("baseUrl", bleConfigBaseUrl);
            bleConfigBaseUrl.toCharArray(baseUrl, 128);
        }
        if (bleConfigToken.length() > 0)
        {
            preferences.putString("faktsToken", bleConfigToken);
            bleConfigToken.toCharArray(faktsToken, 128);
        }

        bool isEnterprise = bleWifiIdentity.length() > 0;
        preferences.putBool("wifiEnterprise", isEnterprise);
        // Always save SSID so connectWiFi() can reconnect after reboot
        preferences.putString("wifiSSID", bleWifiSSID);
        if (isEnterprise)
        {
            preferences.putString("wifiIdentity", bleWifiIdentity);
            preferences.putString("wifiPassword", bleWifiPassword);
            preferences.putString("wifiAnonId", bleWifiAnonIdentity);
            preferences.putString("wifiPemCert", bleWifiPemCertificate);
        }
        else
        {
            // Save password for plain WPA/WPA2 networks
            preferences.putString("wifiPassword", bleWifiPassword);
        }
        preferences.end();
    }

    // ======================== WiFi Connection ========================

    void connectWiFiEnterprise(const String &ssid, const String &identity, const String &password, const String &anonId, const String &pemCert)
    {
        Serial.println("Connecting to WPA2-Enterprise WiFi: " + ssid);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);

        esp_err_t err;
        if (anonId.length() > 0)
        {
            err = esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)anonId.c_str(), anonId.length());
            Serial.println("[WPA2] Set anonymous identity (err=" + String(err) + ")");
        }
        else
        {
            err = esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)identity.c_str(), identity.length());
            Serial.println("[WPA2] Set identity as anonymous (err=" + String(err) + ")");
        }

        err = esp_wifi_sta_wpa2_ent_set_username((uint8_t *)identity.c_str(), identity.length());
        Serial.println("[WPA2] Set username (err=" + String(err) + ")");

        if (password.length() > 0)
        {
            err = esp_wifi_sta_wpa2_ent_set_password((uint8_t *)password.c_str(), password.length());
            Serial.println("[WPA2] Set password: " + String(password.length()) + " chars (err=" + String(err) + ")");
        }

        if (pemCert.length() > 0)
        {
            err = esp_wifi_sta_wpa2_ent_set_ca_cert((uint8_t *)pemCert.c_str(), pemCert.length() + 1);
            Serial.println("[WPA2] Set CA cert: " + String(pemCert.length()) + " bytes (err=" + String(err) + ")");
        }

        err = esp_wifi_sta_wpa2_ent_enable();
        Serial.println("[WPA2] Enabled (err=" + String(err) + ")");
        WiFi.begin(ssid.c_str());
    }

    bool waitForWiFi(unsigned long timeoutMs = 20000)
    {
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeoutMs)
        {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.println("WiFi connected! IP: " + WiFi.localIP().toString());
            return true;
        }
        else
        {
            int status = WiFi.status();
            Serial.println("WiFi failed. Status: " + String(status));
            return false;
        }
    }

    void connectWiFi()
    {
        // Load saved config
        preferences.begin("arkitekt", false);
        String savedBaseUrl = preferences.getString("baseUrl", DEFAULT_BASE_URL);
        savedBaseUrl.toCharArray(baseUrl, 128);

        String savedToken = preferences.getString("faktsToken", DEFAULT_REDEEM_TOKEN);
        savedToken.toCharArray(faktsToken, 128);

        bool savedEnterprise = preferences.getBool("wifiEnterprise", false);
        String savedWifiSSID = preferences.getString("wifiSSID", "");
        String savedWifiIdentity = preferences.getString("wifiIdentity", "");
        String savedWifiPassword = preferences.getString("wifiPassword", "");
        String savedWifiAnonId = preferences.getString("wifiAnonId", "");
        String savedWifiPemCert = preferences.getString("wifiPemCert", "");
        preferences.end();

        // Override from RunConfig if provided
        if (config.redeemToken.length() > 0)
        {
            config.redeemToken.toCharArray(faktsToken, 128);
        }
        if (config.baseUrl.length() > 0)
        {
            config.baseUrl.toCharArray(baseUrl, 128);
        }

        Serial.println("Base URL: " + String(baseUrl));
        Serial.println("Enterprise WiFi: " + String(savedEnterprise ? "yes" : "no"));
        Serial.println("Saved SSID: " + savedWifiSSID);

        // hasStoredWifi: true if we have a saved SSID, the WiFi library has
        // stored credentials, or we're already connected
        bool hasStoredWifi = (WiFi.status() == WL_CONNECTED ||
                              WiFi.SSID().length() > 0 ||
                              savedWifiSSID.length() > 0);

        bool wifiConnected = false;

        if (hasStoredWifi)
        {
            Serial.println("Already provisioned. Connecting...");

            for (uint8_t attempt = 1; attempt <= config.maxWifiRetries; attempt++)
            {
                Serial.println("[WiFi] Attempt " + String(attempt) + "/" + String(config.maxWifiRetries));

                if (savedEnterprise && savedWifiSSID.length() > 0)
                {
                    connectWiFiEnterprise(savedWifiSSID, savedWifiIdentity, savedWifiPassword, savedWifiAnonId, savedWifiPemCert);
                }
                else if (savedWifiSSID.length() > 0)
                {
                    // Use explicitly saved credentials (standard WPA/WPA2)
                    WiFi.begin(savedWifiSSID.c_str(), savedWifiPassword.c_str());
                }
                else
                {
                    // Fall back to WiFi-library stored credentials
                    WiFi.begin();
                }

                if (waitForWiFi())
                {
                    wifiConnected = true;
                    break;
                }

                Serial.println("[WiFi] Attempt " + String(attempt) + " failed");
                WiFi.disconnect(true);
                delay(1000);
            }

            if (!wifiConnected)
            {
                preferences.begin("arkitekt", true);
                bool everConnected = preferences.getBool("wifiEverOk", false);
                preferences.end();

                if (!everConnected && config.ble)
                {
                    Serial.println("[WiFi] Never connected successfully. Falling back to BLE provisioning...");
                    clearSavedConfig();
                }
                else
                {
                    Serial.println("Erasing WiFi config and restarting...");
                    WiFi.disconnect(true, true);
                    ESP.restart();
                }
            }
        }

        if (!wifiConnected && config.ble)
        {
            // BLE provisioning
            startBLEProvisioning();
            waitForBLEConfig();

            bool isEnterprise = bleWifiIdentity.length() > 0;

            if (bleWifiSSID.length() > 0)
            {
                if (isEnterprise)
                {
                    connectWiFiEnterprise(bleWifiSSID, bleWifiIdentity, bleWifiPassword, bleWifiAnonIdentity, bleWifiPemCertificate);
                }
                else if (bleWifiPassword.length() > 0)
                {
                    WiFi.begin(bleWifiSSID.c_str(), bleWifiPassword.c_str());
                }
                else
                {
                    Serial.println("No WiFi credentials provided!");
                    delay(3000);
                    ESP.restart();
                }

                if (!waitForWiFi())
                {
                    if (statusCharacteristic)
                    {
                        statusCharacteristic->setValue("WiFi Failed");
                        statusCharacteristic->notify();
                    }
                    delay(3000);
                    ESP.restart();
                }

                wifiConnected = true;

                if (statusCharacteristic)
                {
                    String status = "Connected: " + WiFi.localIP().toString();
                    statusCharacteristic->setValue(status.c_str());
                    statusCharacteristic->notify();
                }

                delay(2000);
                BLEDevice::deinit();
                Serial.println("BLE stopped");
            }
            else
            {
                Serial.println("No SSID received via BLE!");
                delay(3000);
                ESP.restart();
            }
        }

        if (!wifiConnected && !config.ble)
        {
            Serial.println("No WiFi and BLE disabled. Cannot proceed.");
            delay(3000);
            ESP.restart();
        }

        // Mark that WiFi has connected at least once
        if (wifiConnected)
        {
            preferences.begin("arkitekt", false);
            preferences.putBool("wifiEverOk", true);
            preferences.end();
        }
    }

    // ======================== App Initialization ========================

    bool initializeAppFlow()
    {
        String base = String(baseUrl);
        if (!base.endsWith("/"))
            base += "/";
        claimUrl = base + "lok/f/claim/";

        Serial.println("\n=== Claiming Fakts ===");
        faktsConfig.reset();
        String claimError;

        if (!claimFakts(faktsToken, claimUrl, faktsConfig, claimError))
        {
            Serial.println("Claim failed: " + claimError);
            clearSavedConfig();
            Serial.println("Restarting for BLE provisioning...");
            delay(3000);
            ESP.restart();
            return false;
        }

        Serial.println("Fakts retrieved!");
        faktsConfig.print();

        Serial.println("\n=== OAuth2 Token ===");
        String oauth2Error;
        if (!getOAuth2Token(faktsConfig.auth, accessToken, oauth2Error))
        {
            Serial.println("OAuth2 failed: " + oauth2Error);
            return false;
        }
        Serial.println("Access token acquired.");

        Serial.println("\n=== Initializing App ===");
        app.reset();
        if (!app.initialize(faktsConfig, accessToken))
        {
            Serial.println("App initialization failed");
            return false;
        }
        app.printServices();

        ServiceInstance *rekuest = app.getService("rekuest");
        if (!rekuest || !rekuest->activeAlias)
        {
            Serial.println("Rekuest service not available");
            return false;
        }

        // Create agent
        Serial.println("\n=== Creating Agent ===");
        agent = new Agent(&app, "rekuest", instanceId, agentName);
        agent->setWebSocket(&webSocket);

        // Re-register all user-defined functions and states on the agent
        for (auto &pair : pendingFunctions)
        {
            agent->registerFunction(pair.first, pair.second.first, pair.second.second);
        }
        for (auto &pair : pendingStates)
        {
            agent->registerState(pair.first, pair.second);
        }

        // Initialize pending states
        for (auto &pair : pendingStateInits)
        {
            AgentState *state = agent->getState(pair.first);
            if (state)
            {
                pair.second(state);
            }
        }

        agent->printRegistry();

        // Ensure agent on server
        String agentError;
        DynamicJsonDocument extensionsDoc(256);
        JsonArray extensions = extensionsDoc.to<JsonArray>();
        extensions.add("default");

        if (!agent->ensureAgent(agentName, extensions, agentError))
        {
            Serial.println("Failed to ensure agent: " + agentError);
            return false;
        }

        // Setup WebSocket
        setupWebSocket(rekuest);

        Serial.println("\n=== App Ready ===");
        Serial.println("Listening for assignments over WebSocket...");
        return true;
    }

    void clearSavedConfig()
    {
        preferences.begin("arkitekt", false);
        preferences.clear();
        preferences.end();
        WiFi.disconnect(true, true);
    }

    // ======================== WebSocket ========================

    void setupWebSocket(ServiceInstance *service)
    {
        Alias *alias = service->activeAlias;
        if (!alias)
            return;

        uint16_t port = (alias->port != -1)
                            ? static_cast<uint16_t>(alias->port)
                            : static_cast<uint16_t>(alias->ssl ? 443 : 80);

        String path = alias->path;
        if (path.length() == 0)
            path = "/";
        else if (!path.startsWith("/"))
            path = "/" + path;
        if (!path.endsWith("/"))
            path += "/";
        path += "agi";

        webSocket.onEvent([this](WStype_t type, uint8_t *payload, size_t length)
                          { this->onWebSocketEvent(type, payload, length); });
        webSocket.setReconnectInterval(5000);
        webSocket.enableHeartbeat(15000, 3000, 2);

        Serial.println("WebSocket connecting to " + alias->host + ":" + String(port) + path);

        if (alias->ssl)
            webSocket.beginSSL(alias->host.c_str(), port, path.c_str());
        else
            webSocket.begin(alias->host.c_str(), port, path.c_str());
    }

    static String payloadToString(uint8_t *payload, size_t length)
    {
        String msg;
        msg.reserve(length);
        for (size_t i = 0; i < length; ++i)
            msg += static_cast<char>(payload[i]);
        return msg;
    }

    void sendHeartbeatAnswer()
    {
        StaticJsonDocument<256> doc;
        doc["type"] = "HEARTBEAT_ANSWER";
        doc["id"] = replyGenerateUUID4();
        String msg;
        serializeJson(doc, msg);
        webSocket.sendTXT(msg);
    }

    void onWebSocketEvent(WStype_t type, uint8_t *payload, size_t length)
    {
        switch (type)
        {
        case WStype_CONNECTED:
        {
            Serial.println("WebSocket connected");
            if (agent)
            {
                agent->beginSession();
            }

            StaticJsonDocument<512> doc;
            doc["type"] = "REGISTER";
            doc["instance_id"] = instanceId;
            doc["token"] = accessToken;
            String msg;
            serializeJson(doc, msg);
            Serial.println("WS >> " + msg);
            webSocket.sendTXT(msg);
            break;
        }

        case WStype_DISCONNECTED:
            if (length > 0)
                Serial.printf("WebSocket disconnected (code: %d, reason: %s)\n", (int)payload[0], (char *)payload);
            else
                Serial.println("WebSocket disconnected (no reason given)");
            if (agent)
                agent->resetSession();
            break;

        case WStype_TEXT:
        {
            String message = payloadToString(payload, length);
            Serial.println("WS << " + message);

            String trimmed = message;
            trimmed.trim();

            if (trimmed.equalsIgnoreCase("HEARTBEAT"))
            {
                sendHeartbeatAnswer();
                return;
            }

            DynamicJsonDocument doc(1024);
            DeserializationError err = deserializeJson(doc, message);
            if (err)
                return;

            const char *typeField = doc["type"] | "";
            String typeStr(typeField);
            typeStr.toUpperCase();

            if (typeStr == "HEARTBEAT")
            {
                sendHeartbeatAnswer();
            }
            else if (typeStr == "INIT")
            {
                Serial.println("Server acknowledged REGISTER");
                if (agent)
                {
                    agent->sendSessionInit();
                    agent->sendStateSnapshot();
                }
            }
            else if (typeStr == "ASSIGN")
            {
                Serial.println("\n=== Received Assignment ===");
                if (agent)
                {
                    String interfaceName = doc["interface"] | "";
                    String assignation = doc["assignation"] | "";

                    Serial.println("Interface: " + interfaceName);
                    Serial.println("Assignation: " + assignation);

                    JsonObject args = doc["args"].as<JsonObject>();

                    bool success = agent->handleAssignment(*this, interfaceName, assignation, args);

                    if (!success)
                    {
                        // If the user callback returned false without sending CRITICAL, send one
                        ReplyChannel reply(&webSocket, assignation);
                        reply.critical("Function execution failed");
                    }
                }
            }
            break;
        }

        case WStype_ERROR:
            Serial.println("WebSocket error");
            break;

        default:
            break;
        }
    }

    // ======================== Factory Reset ========================

    void checkFactoryReset()
    {
        if (digitalRead(0) == LOW)
        {
            if (!bootButtonHeld)
            {
                bootButtonHeld = true;
                bootButtonPressStart = millis();
                Serial.println("[RESET] BOOT button pressed...");
            }
            else if (millis() - bootButtonPressStart >= config.bootReconfigureTimeout)
            {
                Serial.println("\n[RESET] Factory reset triggered!");
                clearSavedConfig();
                Serial.println("[RESET] Done. Restarting...");
                delay(1000);
                ESP.restart();
            }
        }
        else
        {
            if (bootButtonHeld)
                Serial.println("[RESET] BOOT button released.");
            bootButtonHeld = false;
        }
    }

    // ======================== Pending registrations ========================
    // These hold user registrations before agent is created

    std::map<String, std::pair<FunctionDefinition, AgentFunction>> pendingFunctions;
    std::map<String, StateDefinition> pendingStates;
    std::map<String, std::function<void(AgentState *)>> pendingStateInits;

public:
    ArkitektApp(const String &identifier, const String &version, const String &instId, const String &name)
        : instanceId(instId), agentName(name), manifest(identifier, version),
          agent(nullptr), appReady(false),
          pServer(nullptr), statusCharacteristic(nullptr),
          deviceConnected(false), oldDeviceConnected(false),
          hasNewConfig(false), bootButtonPressStart(0), bootButtonHeld(false)
    {
        memset(baseUrl, 0, sizeof(baseUrl));
        strncpy(baseUrl, DEFAULT_BASE_URL, sizeof(baseUrl) - 1);
        memset(faktsToken, 0, sizeof(faktsToken));
        strncpy(faktsToken, DEFAULT_REDEEM_TOKEN, sizeof(faktsToken) - 1);
        _arkitektAppInstance = this;
    }

    // ======================== Registration API ========================

    void addRequirement(const String &key, const String &service, bool optional = false)
    {
        manifest.addRequirement(key, service, optional);
    }

    void addScope(const String &scope)
    {
        manifest.addScope(scope);
    }

    void registerFunction(const String &name, const FunctionDefinition &definition, AgentFunction callback)
    {
        pendingFunctions[name] = std::make_pair(definition, callback);
        Serial.println("Registered function: " + name);
    }

    void registerState(const String &name, const StateDefinition &definition)
    {
        pendingStates[name] = definition;
        Serial.println("Registered state: " + name);
    }

    void registerState(const String &name, const StateDefinition &definition, std::function<void(AgentState *)> initFn)
    {
        pendingStates[name] = definition;
        pendingStateInits[name] = initFn;
        Serial.println("Registered state: " + name);
    }

    void registerBackgroundTask(std::function<void(ArkitektApp &, Agent &)> callback, unsigned long intervalMs = 1000)
    {
        backgroundTasks.push_back(BackgroundTask(callback, intervalMs));
        Serial.println("Registered background task (interval: " + String(intervalMs) + "ms)");
    }

    // ======================== Accessors ========================

    Agent *getAgent() { return agent; }
    App &getApp() { return app; }
    WebSocketsClient &getWebSocket() { return webSocket; }

    AgentState *getState(const String &name)
    {
        if (agent)
            return agent->getState(name);
        return nullptr;
    }

    ServiceInstance *getService(const String &key)
    {
        return app.getService(key);
    }

    // ======================== Run / Loop ========================

    void run(const RunConfig &cfg = RunConfig())
    {
        config = cfg;

        Serial.println("\n=== Arkitekt App Starting ===");

        // Setup factory reset button
        pinMode(0, INPUT_PULLUP);
        Serial.println("[RESET] Hold BOOT button for " + String(config.bootReconfigureTimeout / 1000) + "s to factory reset.");

        // Generate device ID and agent name from MAC address
        generateDeviceId();
        generateAgentName();
        manifest.print();

        // Connect WiFi (BLE provisioning if needed)
        connectWiFi();

        // Initialize the full flow: claim fakts, OAuth2, agent, WebSocket
        appReady = initializeAppFlow();

        if (!appReady)
        {
            Serial.println("App initialization failed. Check logs above.");
        }
    }

    void loop()
    {
        checkFactoryReset();

        if (appReady)
        {
            webSocket.loop();

            // Run background tasks
            if (agent)
            {
                for (auto &task : backgroundTasks)
                {
                    if (millis() - task.lastRun >= task.intervalMs)
                    {
                        task.lastRun = millis();
                        task.callback(*this, *agent);
                    }
                }
            }
        }
    }
};

#endif // ARKITEKT_APP_H
