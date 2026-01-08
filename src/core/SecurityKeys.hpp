/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef SECURITY_KEYS_H
#define SECURITY_KEYS_H

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include <ConfigNvs.hpp>
#include <WiFi.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "mbedtls/sha256.h"

static inline bool derivePmkFromMasterMac_(const uint8_t masterMac[6], uint8_t out[16]) {
    if (!masterMac || !out) {
        return false;
    }
    static const char salt[] = "PMK-V1";
    uint8_t seed[6 + sizeof(salt) - 1] = {0};
    memcpy(seed, masterMac, 6);
    memcpy(seed + 6, salt, sizeof(salt) - 1);

    uint8_t hash[32] = {0};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts_ret(&ctx, 0) == 0) {
        mbedtls_sha256_update_ret(&ctx, seed, sizeof(seed));
        mbedtls_sha256_finish_ret(&ctx, hash);
    }
    mbedtls_sha256_free(&ctx);
    memcpy(out, hash, 16);
    return true;
}

static inline bool deriveLmkFromMacs_(const uint8_t masterMac[6], const uint8_t slaveMac[6], uint8_t out[16]) {
    if (!masterMac || !slaveMac || !out) {
        return false;
    }
    static const char salt[] = "LMK-V1";
    uint8_t seed[12 + sizeof(salt) - 1] = {0};
    memcpy(seed, masterMac, 6);
    memcpy(seed + 6, slaveMac, 6);
    memcpy(seed + 12, salt, sizeof(salt) - 1);

    uint8_t hash[32] = {0};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts_ret(&ctx, 0) == 0) {
        mbedtls_sha256_update_ret(&ctx, seed, sizeof(seed));
        mbedtls_sha256_finish_ret(&ctx, hash);
    }
    mbedtls_sha256_free(&ctx);
    memcpy(out, hash, 16);
    return true;
}

static inline String generateMasterPmk_() {
    uint8_t mac[6] = {0};
    esp_err_t ret = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (ret != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }

    static const char salt[] = "PMK-V1";
    uint8_t seed[6 + sizeof(salt) - 1] = {0};
    memcpy(seed, mac, 6);
    memcpy(seed + 6, salt, sizeof(salt) - 1);

    uint8_t hash[32] = {0};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts_ret(&ctx, 0) == 0) {
        mbedtls_sha256_update_ret(&ctx, seed, sizeof(seed));
        mbedtls_sha256_finish_ret(&ctx, hash);
    }
    mbedtls_sha256_free(&ctx);

    char pmkHex[33] = {0};
    for (int i = 0; i < 16; ++i) {
        snprintf(pmkHex + (i * 2), 3, "%02X", hash[i]);
    }
    pmkHex[32] = '\0';
    return String(pmkHex);
}

static inline String generateSlaveLmk_(const uint8_t slaveMac[6]) {
    uint8_t masterMac[6] = {0};
    esp_err_t ret = esp_wifi_get_mac(WIFI_IF_AP, masterMac);
    if (ret != ESP_OK) {
        memset(masterMac, 0, sizeof(masterMac));
    }

    static const char salt[] = "LMK-V1";
    uint8_t seed[12 + sizeof(salt) - 1] = {0};
    memcpy(seed, masterMac, 6);
    memcpy(seed + 6, slaveMac, 6);
    memcpy(seed + 12, salt, sizeof(salt) - 1);

    uint8_t hash[32] = {0};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts_ret(&ctx, 0) == 0) {
        mbedtls_sha256_update_ret(&ctx, seed, sizeof(seed));
        mbedtls_sha256_finish_ret(&ctx, hash);
    }
    mbedtls_sha256_free(&ctx);

    char lmkHex[33] = {0};
    for (int i = 0; i < 16; ++i) {
        snprintf(lmkHex + (i * 2), 3, "%02X", hash[i]);
    }
    lmkHex[32] = '\0';
    return String(lmkHex);
}

static inline String generateDeviceId_() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toUpperCase();
    String macTail = mac.substring(6);

    const uint64_t efuse = ESP.getEfuseMac();
    char efuseBuf[13] = {0};
    snprintf(efuseBuf, sizeof(efuseBuf), "%010llX",
             static_cast<unsigned long long>(efuse));
    String efTail = String(efuseBuf);
    if (efTail.length() > 6) {
        efTail = efTail.substring(efTail.length() - 6);
    }

    return macTail + efTail;
}

static inline void generateDeviceNames_(String& deviceNameOut, String& configNameOut) {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toUpperCase();
    String macTail = mac.substring(6);

    const char* letters[] = { "X", "W", "Z", "Q", "J" };
    String fused;
    for (int i = 0; i < macTail.length(); i += 2) {
        String pair = macTail.substring(i, i + 2);
        String randChar = letters[random(0, 5)];
        fused += pair + randChar;
    }

    deviceNameOut = String(DEVICE_NAME_DEFAULT) + "_" + fused;
    configNameOut = String(DEVICE_NAME_DEFAULT) + "_" + fused;
}

#endif // SECURITY_KEYS_H
