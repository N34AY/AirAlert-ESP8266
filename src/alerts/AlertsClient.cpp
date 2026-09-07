#include "AlertsClient.h"
#include "AlertsCa.h"
#ifdef AIRALERT_DEV
#include "CaBundle.h"
#endif

// SPEC 133: production hostname is immutable. Dev builds may point at a local
// emulator at runtime (serial `setmock`), never via any web UI.
static const char kAlertsHost[] = "api.ukrainealarm.com";
static const char kAlertsUrl[] = "https://api.ukrainealarm.com/api/v3/alerts";

// Constant-memory body parse: the response is a top-level array of region
// entries, deserialized ONE ENTRY at a time, so heap use does not grow with
// the nationwide alert count. A whole-document parse hit
// DeserializationError::NoMemory on busy nights (~20 KB body with ~25 KB free
// heap while TLS buffers are live) and showed up as a permanent
// API_PARSE_ERROR streak.
AlertsClient::Result::Kind AlertsClient::parseBody(Stream& in,
                                                   airalert::SnapshotBuilder& builder,
                                                   Result& r) {
    // Two limits: 20 s total for a busy-night body (20-35 KB over a weak
    // link) and 5 s without a single byte, so a dead connection fails fast
    // while a slow one that still delivers is allowed to finish. loop() is
    // blocked meanwhile; main never starts a poll while the relay is ON.
    const uint32_t start = millis();
    uint32_t lastByteAt = start;
    const auto timedOut = [&] {
        const uint32_t now = millis();
        return now - start >= 20000 || now - lastByteAt >= 5000;
    };

    // 1) scan to the opening '[' of the region array
    bool inArray = false;
    while (!timedOut()) {
        const int c = in.read();
        if (c < 0) {
            if (!in.available()) delay(1);
            continue;
        }
        lastByteAt = millis();
        if (c == '[') { inArray = true; break; }
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') break; // not an array body
    }
    if (!inArray) {
        r.parseDetail = "NoAlertsArray";
        return Result::Kind::ParseError; // SPEC 167
    }

    builder.reset();
    JsonDocument filter;
    airalert::buildAlertElementFilter(filter);

    // 2) element loop: {..},{..}] — one small filtered doc per region entry
    while (!timedOut()) {
        int c = in.peek();
        if (c < 0) { delay(1); continue; }
        lastByteAt = millis();
        if (c == ']') { in.read(); return Result::Kind::Ok; }
        if (c == ',' || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            in.read();
            continue;
        }
        JsonDocument doc;
        const auto err = deserializeJson(doc, in, DeserializationOption::Filter(filter));
        if (err != DeserializationError::Ok) {
            r.parseDetail = err.c_str(); // static string from ArduinoJson
            r.heapAtError = ESP.getFreeHeap();
            return Result::Kind::ParseError;
        }
        airalert::regionToAlerts(doc.as<JsonVariantConst>(), builder, r.stats);
        yield();
    }
    r.parseDetail = "timeout";
    return Result::Kind::ParseError;
}

AlertsClient::Result AlertsClient::poll(airalert::SnapshotBuilder& builder) {
    Result r;

    const char* url = kAlertsUrl;
    BearSSL::WiFiClientSecure secureClient;
    WiFiClient* client = &secureClient;
#ifdef AIRALERT_DEV
    bool devMockTls = false;
    WiFiClient plainClient;
    if (mockUrl_.length()) {
        url = mockUrl_.c_str();
        if (mockUrl_.startsWith("https")) {
            devMockTls = true;
        } else {
            client = &plainClient;
        }
    }
#endif
    if (client == &secureClient) {
#ifdef AIRALERT_DEV
        if (devMockTls) {
            static BearSSL::X509List devCas(CA_BUNDLE_PEM);
            secureClient.setTrustAnchors(&devCas);
        } else
#endif
        {
            static BearSSL::X509List ca(ALERTS_CA_PEM);
            secureClient.setTrustAnchors(&ca);
            secureClient.setSession(&session_);
        }
        // MFLN probe (once): if the server honors 1 KB TLS fragments, the RX
        // buffer drops 4096->1024 and the handshake peak needs ~3 KB less -
        // that margin is the difference between polling and code=-1 on a
        // ~25 KB heap. Falls back to 4096 when unsupported.
        static int8_t mfln = -1; // -1 unknown, 0 no, 1 yes
        if (mfln < 0 && url == kAlertsUrl)
            mfln = BearSSL::WiFiClientSecure::probeMaxFragmentLength(
                       kAlertsHost, 443, 1024) ? 1 : 0;
        secureClient.setBufferSizes(mfln == 1 ? 1024 : 4096, 512); // SPEC 113
    }

    HTTPClient http;
    http.setTimeout(10000);
    http.useHTTP10(true); // no chunked encoding -> ArduinoJson can read the stream
    if (!http.begin(*client, url)) { r.kind = Result::Kind::NetError; return r; }

    http.addHeader("Authorization", token_); // ukrainealarm: raw key, no scheme
    if (lastModified_.length()) http.addHeader("If-Modified-Since", lastModified_);
    const char* keys[] = {"Last-Modified", "Retry-After"};
    http.collectHeaders(keys, 2);

    const int code = http.GET();
    r.httpCode = code;

    switch (code) {
        case HTTP_CODE_OK: {
            const String responseLastModified = http.header("Last-Modified");
            r.kind = parseBody(http.getStream(), builder, r);
            if (r.kind != Result::Kind::Ok) {
                // The builder holds a half-streamed snapshot and we no longer
                // own a validated one: a later 304 must not resurrect it
                // (Invariant 6). Drop the validator too, so the next poll
                // asks for a full body.
                invalidateCache();
                break;
            }
            lastModified_ = responseLastModified;
            cache_.commitValidSnapshot();
            break;
        }
        case HTTP_CODE_NOT_MODIFIED:
            if (cache_.canAcceptNotModified()) {
                r.kind = Result::Kind::NotModified;
            } else {
                // A 304 without a validated snapshot cannot establish state.
                lastModified_ = "";
                r.kind = Result::Kind::ParseError;
            }
            break;
        case HTTP_CODE_UNAUTHORIZED: r.kind = Result::Kind::AuthError; break;   // bad key OR rate limit: policy in doPoll()
        case HTTP_CODE_FORBIDDEN: r.kind = Result::Kind::Forbidden; break;      // SPEC 164
        case HTTP_CODE_TOO_MANY_REQUESTS:                                        // SPEC 165
            r.kind = Result::Kind::RateLimited;
            r.retryAfterSec = http.header("Retry-After").toInt();
            break;
        default:
            r.kind = code < 0 ? Result::Kind::NetError : Result::Kind::HttpError;
            break;
    }
    http.end();
    return r;
}
