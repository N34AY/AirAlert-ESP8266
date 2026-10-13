#pragma once
// /secrets.json - Wi-Fi, API token, admin password hash (SPEC 91, 176).
// Values never leave the device; REST exposes only presence flags.
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <bearssl/bearssl_hash.h>
#include <osapi.h>
#include "AtomicJsonFile.h"

struct SecretsStore {
    String wifiSsid, wifiPass, apiToken;
    String webPassHash, webSalt; // hex(sha256(salt+password)), hex salt
    String mockUrl;              // LAN proxy / emulator URL (`setapi`), empty = official API
    String provisioningPass;

    static constexpr const char* kPath = "/secrets.json";
    static constexpr const char* kTmpPath = "/secrets.new";
    static constexpr const char* kBackupPath = "/secrets.bak";

    bool valid() const {
        const bool passwordEmpty = webPassHash.length() == 0 && webSalt.length() == 0;
        const bool passwordSet = webPassHash.length() == 64 && webSalt.length() == 16;
        const bool mockUrlValid = mockUrl.length() == 0 ||
            (mockUrl.length() <= 192 &&
             (mockUrl.startsWith("http://") || mockUrl.startsWith("https://")));
        return wifiSsid.length() <= 32 && wifiPass.length() <= 64 &&
               (wifiSsid.length() > 0 || wifiPass.length() == 0) &&
               (apiToken.length() == 0 ||
                (apiToken.length() >= 10 && apiToken.length() <= 256)) &&
               (passwordEmpty || passwordSet) &&
               mockUrlValid &&
               (provisioningPass.length() == 0 ||
                (provisioningPass.length() >= 12 && provisioningPass.length() <= 32));
    }

    bool load() {
        JsonDocument d;
        if (!atomic_json::readWithBackup(kPath, kBackupPath, d)) return false;
        SecretsStore next = *this;
        next.wifiSsid = d["wifi_ssid"] | "";
        next.wifiPass = d["wifi_pass"] | "";
        next.apiToken = d["api_token"] | "";
        next.webPassHash = d["web_pass_hash"] | "";
        next.webSalt = d["web_salt"] | "";
        next.provisioningPass = d["provisioning_pass"] | "";
        next.mockUrl = d["mock_url"] | "";
        if (!next.valid()) return false;
        *this = next;
        return true;
    }

    bool save() {
        if (!valid()) return false;
        JsonDocument d;
        d["wifi_ssid"] = wifiSsid;
        d["wifi_pass"] = wifiPass;
        d["api_token"] = apiToken;
        d["web_pass_hash"] = webPassHash;
        d["web_salt"] = webSalt;
        if (mockUrl.length()) d["mock_url"] = mockUrl;
        d["provisioning_pass"] = provisioningPass;
        return atomic_json::write(kPath, kTmpPath, kBackupPath, d);
    }

    static String sha256Hex(const String& in) {
        br_sha256_context ctx;
        uint8_t digest[32];
        br_sha256_init(&ctx);
        br_sha256_update(&ctx, in.c_str(), in.length());
        br_sha256_out(&ctx, digest);
        String out;
        out.reserve(64);
        for (uint8_t b : digest) {
            char h[3];
            snprintf(h, 3, "%02x", b);
            out += h;
        }
        return out;
    }

    void setWebPassword(const String& pass) {
        uint8_t bytes[8];
        os_get_random(bytes, sizeof bytes);
        char salt[17];
        for (uint8_t i = 0; i < sizeof bytes; ++i)
            snprintf(salt + i * 2, 3, "%02x", bytes[i]);
        webSalt = salt;
        webPassHash = sha256Hex(webSalt + pass); // no plaintext stored (SPEC 100)
    }

    bool checkWebPassword(const String& pass) const {
        if (!webPassHash.length()) return false;
        const String candidate = sha256Hex(webSalt + pass);
        if (candidate.length() != webPassHash.length()) return false;
        uint8_t different = 0;
        for (size_t i = 0; i < candidate.length(); ++i)
            different |= static_cast<uint8_t>(candidate[i] ^ webPassHash[i]);
        return different == 0;
    }

    bool hasWebPassword() const { return webPassHash.length() > 0; }
};
