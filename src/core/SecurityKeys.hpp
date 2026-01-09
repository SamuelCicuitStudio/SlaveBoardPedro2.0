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
#include "mbedtls/md.h"              // HMAC-SHA256
#include "mbedtls/platform_util.h"   // for mbedtls_platform_zeroize

#ifndef ESPNOW_PMK_HEX
#define ESPNOW_PMK_HEX "A7F3C91D4E2B86A0D5C8F1B9047E3A6C"
#endif

// Security constants
#define SECRET_KEY "indulock" // Static secret string (used as HMAC key)

/**
 * Deterministic, keyed LMK derivation:
 *   LMK = Trunc16( HMAC-SHA256( key=SECRET_KEY, msg=masterMac||seed||"LMK-V2" ) )
 *
 * - Deterministic: same inputs => same output
 * - Robust: checks all errors, never returns true on failure
 * - Keyed: cannot be recomputed without SECRET_KEY
 */
static inline bool deriveLmkFromSeed_(const uint8_t masterMac[6],
                                      uint32_t seed,
                                      uint8_t out[16])
{
    if (masterMac == NULL || out == NULL) {
        return false;
    }

    // Bump this when you change any part of the derivation
    static const uint8_t salt[] = { 'L','M','K','-','V','2' };

    const uint8_t seedBytes[4] = {
        (uint8_t)(seed >> 24),
        (uint8_t)(seed >> 16),
        (uint8_t)(seed >>  8),
        (uint8_t)(seed >>  0),
    };

    // msg = masterMac(6) || seed(4, BE) || salt(6)
    uint8_t msg[6 + 4 + sizeof(salt)];
    memcpy(msg,      masterMac, 6);
    memcpy(msg + 6,  seedBytes, 4);
    memcpy(msg + 10, salt,      sizeof(salt));

    const uint8_t* key = reinterpret_cast<const uint8_t*>(SECRET_KEY);
    const size_t   keyLen = strlen(SECRET_KEY);
    if (keyLen == 0) {
        mbedtls_platform_zeroize(msg, sizeof(msg));
        return false;
    }

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        mbedtls_platform_zeroize(msg, sizeof(msg));
        return false;
    }

    uint8_t mac[32];
    const int rc = mbedtls_md_hmac(info, key, keyLen, msg, sizeof(msg), mac);
    if (rc != 0) {
        mbedtls_platform_zeroize(msg, sizeof(msg));
        mbedtls_platform_zeroize(mac, sizeof(mac));
        return false;
    }

    memcpy(out, mac, 16);

    mbedtls_platform_zeroize(msg, sizeof(msg));
    mbedtls_platform_zeroize(mac, sizeof(mac));
    return true;
}

// Helper: get 12-hex uppercase string of the eFuse MAC (48-bit)
static inline String efuseMacHex12_() {
    const uint64_t efuse = ESP.getEfuseMac();

    // ESP.getEfuseMac() returns a 64-bit value, but the MAC is effectively 48 bits.
    const uint64_t mac48 = (efuse & 0xFFFFFFFFFFFFULL);

    char buf[13]; // 12 hex + NUL
    snprintf(buf, sizeof(buf), "%012llX", (unsigned long long)mac48);
    return String(buf); // already uppercase due to %X
}

// Deterministic Device ID: last 6 hex of eFuse MAC + last 6 hex again (or change pattern as you like)
static inline String generateDeviceId_() {
    const String mac12 = efuseMacHex12_();      // e.g. "A1B2C3D4E5F6"
    const String tail6 = mac12.substring(6);    // e.g. "D4E5F6"

    // If you want exactly 12 chars like before, keep it simple:
    // return tail6 + tail6;

    // Or, a slightly “mixed” deterministic 12 chars:
    const String head6 = mac12.substring(0, 6); // e.g. "A1B2C3"
    return tail6 + head6;                       // e.g. "D4E5F6A1B2C3"
}

// Deterministic names derived from eFuse MAC (no random())
static inline void generateDeviceNames_(String& deviceNameOut, String& configNameOut) {
    const String mac12 = efuseMacHex12_();   // "A1B2C3D4E5F6"

    // Same letter set you used, but deterministic:
    static const char* letters[] = { "X", "W", "Z", "Q", "J" };

    String fused;
    fused.reserve((mac12.length() / 2) * 3); // "AA" + "X" repeated

    for (int i = 0; i < mac12.length(); i += 2) {
        const String pair = mac12.substring(i, i + 2);  // two hex chars
        const uint8_t byteVal = (uint8_t) strtoul(pair.c_str(), nullptr, 16);
        const char* detChar = letters[byteVal % 5];
        fused += pair;
        fused += detChar;
    }

    deviceNameOut = String(DEVICE_NAME_DEFAULT) + "_" + fused;
    configNameOut = String(DEVICE_NAME_DEFAULT) + "_" + fused;
}

#endif // SECURITY_KEYS_H
