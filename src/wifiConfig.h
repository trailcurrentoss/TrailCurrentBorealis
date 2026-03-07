#pragma once
#include <Preferences.h>
#include <Arduino.h>
#include <debug.h>

#define WIFI_CONFIG_NAMESPACE "wifi_config"
#define WIFI_SSID_KEY "ssid"
#define WIFI_PASSWORD_KEY "password"
#define WIFI_CONFIG_TIMEOUT_MS 5000

namespace wifiConfig {
    Preferences preferences;

    char* runtimeSsidPtr = nullptr;
    char* runtimePasswordPtr = nullptr;

    struct {
        bool receiving = false;
        uint8_t ssidLen = 0;
        uint8_t passwordLen = 0;
        uint8_t ssidChunks = 0;
        uint8_t passwordChunks = 0;
        uint8_t receivedSsidChunks = 0;
        uint8_t receivedPasswordChunks = 0;
        char ssidBuffer[33] = {0};
        char passwordBuffer[64] = {0};
        uint8_t expectedChecksum = 0;
        unsigned long lastMessageTime = 0;
    } state;

    void setRuntimeCredentialPtrs(char* ssidPtr, char* passwordPtr) {
        runtimeSsidPtr = ssidPtr;
        runtimePasswordPtr = passwordPtr;
    }

    void init() {
        preferences.begin(WIFI_CONFIG_NAMESPACE, false);
        debugln("[WiFi Config] NVS initialized");
    }

    bool loadCredentials(char* ssid, char* password) {
        if (!preferences.isKey(WIFI_SSID_KEY) || !preferences.isKey(WIFI_PASSWORD_KEY)) {
            debugln("[WiFi Config] No stored credentials found");
            return false;
        }

        String ssidStr = preferences.getString(WIFI_SSID_KEY, "");
        String passwordStr = preferences.getString(WIFI_PASSWORD_KEY, "");

        if (ssidStr.length() == 0) {
            debugln("[WiFi Config] Empty SSID in NVS");
            return false;
        }

        strncpy(ssid, ssidStr.c_str(), 32);
        ssid[32] = '\0';
        strncpy(password, passwordStr.c_str(), 63);
        password[63] = '\0';

        debugf("[WiFi Config] Loaded SSID: %s\n", ssid);
        return true;
    }

    bool saveCredentials(const char* ssid, const char* password) {
        debugf("[WiFi Config] Saving SSID: %s\n", ssid);
        preferences.putString(WIFI_SSID_KEY, ssid);
        preferences.putString(WIFI_PASSWORD_KEY, password);
        debugln("[WiFi Config] Credentials saved to NVS");

        if (runtimeSsidPtr && runtimePasswordPtr) {
            strncpy(runtimeSsidPtr, ssid, 32);
            runtimeSsidPtr[32] = '\0';
            strncpy(runtimePasswordPtr, password, 63);
            runtimePasswordPtr[63] = '\0';
            debugln("[WiFi Config] Runtime credentials updated");
        }

        return true;
    }

    void handleStartMessage(const uint8_t* data) {
        debugln("[WiFi Config] Start message received");
        memset(&state, 0, sizeof(state));
        state.receiving = true;
        state.ssidLen = data[1];
        state.passwordLen = data[2];
        state.ssidChunks = data[3];
        state.passwordChunks = data[4];
        state.lastMessageTime = millis();

        debugf("[WiFi Config] Expecting SSID: %d bytes in %d chunks\n",
               state.ssidLen, state.ssidChunks);
        debugf("[WiFi Config] Expecting Password: %d bytes in %d chunks\n",
               state.passwordLen, state.passwordChunks);
    }

    void handleSsidChunk(const uint8_t* data) {
        if (!state.receiving) return;

        uint8_t chunkIndex = data[1];
        uint8_t offset = chunkIndex * 6;

        if (chunkIndex >= state.ssidChunks) return;

        for (int i = 0; i < 6 && (offset + i) < state.ssidLen; i++) {
            state.ssidBuffer[offset + i] = data[2 + i];
        }

        state.receivedSsidChunks++;
        state.lastMessageTime = millis();
    }

    void handlePasswordChunk(const uint8_t* data) {
        if (!state.receiving) return;

        uint8_t chunkIndex = data[1];
        uint8_t offset = chunkIndex * 6;

        if (chunkIndex >= state.passwordChunks) return;

        for (int i = 0; i < 6 && (offset + i) < state.passwordLen; i++) {
            state.passwordBuffer[offset + i] = data[2 + i];
        }

        state.receivedPasswordChunks++;
        state.lastMessageTime = millis();
    }

    void handleEndMessage(const uint8_t* data) {
        if (!state.receiving) return;

        uint8_t receivedChecksum = data[1];

        if (state.receivedSsidChunks != state.ssidChunks ||
            state.receivedPasswordChunks != state.passwordChunks) {
            debugf("[WiFi Config] ERROR: Missing chunks (SSID: %d/%d, Password: %d/%d)\n",
                   state.receivedSsidChunks, state.ssidChunks,
                   state.receivedPasswordChunks, state.passwordChunks);
            state.receiving = false;
            return;
        }

        uint8_t checksum = 0;
        for (int i = 0; i < state.ssidLen; i++) {
            checksum ^= state.ssidBuffer[i];
        }
        for (int i = 0; i < state.passwordLen; i++) {
            checksum ^= state.passwordBuffer[i];
        }

        if (checksum != receivedChecksum) {
            debugf("[WiFi Config] ERROR: Checksum mismatch (expected: 0x%02X, got: 0x%02X)\n",
                   receivedChecksum, checksum);
            state.receiving = false;
            return;
        }

        state.ssidBuffer[state.ssidLen] = '\0';
        state.passwordBuffer[state.passwordLen] = '\0';

        saveCredentials(state.ssidBuffer, state.passwordBuffer);
        state.receiving = false;
    }

    void handleCanMessage(const uint8_t* data, uint8_t length) {
        if (length < 1) return;

        switch (data[0]) {
            case 0x01: handleStartMessage(data); break;
            case 0x02: handleSsidChunk(data); break;
            case 0x03: handlePasswordChunk(data); break;
            case 0x04: handleEndMessage(data); break;
            default:
                debugf("[WiFi Config] Unknown message type: 0x%02X\n", data[0]);
        }
    }

    void checkTimeout() {
        if (state.receiving && (millis() - state.lastMessageTime > WIFI_CONFIG_TIMEOUT_MS)) {
            debugln("[WiFi Config] Timeout - resetting state");
            memset(&state, 0, sizeof(state));
        }
    }
}
