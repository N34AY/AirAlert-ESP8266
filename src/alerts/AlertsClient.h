#pragma once
// HTTP/TLS transport for api.ukrainealarm.com (SPEC 174: transport only, no policy).
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include "airalert/AlertsParser.h"
#include "airalert/ApiSnapshotCache.h"
#include "airalert/SnapshotBuilder.h"

class AlertsClient {
public:
    struct Result {
        enum class Kind : uint8_t {
            Ok, NotModified, AuthError, Forbidden, RateLimited,
            HttpError, NetError, ParseError
        };
        Kind kind = Kind::NetError;
        int httpCode = 0;
        uint32_t retryAfterSec = 0;
        airalert::ParseStats stats;
        const char* parseDetail = nullptr; // static string, set on ParseError
        uint32_t heapAtError = 0;
    };

    void begin(const String& token) {
        if (token != token_) invalidateCache();
        token_ = token;
    }
    bool hasToken() const { return token_.length() > 0; }
    void invalidateCache() {
        lastModified_ = "";
        cache_.invalidate();
    }
    bool hasCachedSnapshot() const { return cache_.canAcceptNotModified(); }

    // Poll a LAN proxy (proxy/README.md) or a dev emulator instead of
    // production. Serial-only (`setapi <url>`), never via the Web UI (SPEC 133).
    void setMockUrl(const String& url) {
        if (url != mockUrl_) invalidateCache();
        mockUrl_ = url;
    }
    bool mocked() const { return mockUrl_.length() > 0; }
    // Selected location uids ("14,703"), sent to the proxy as ?regions= so it
    // returns only what this device needs. The proxy's Last-Modified does not
    // depend on it, so a new set must drop the 304 validator.
    void setRegions(const String& ids) {
        if (ids != regions_) invalidateCache();
        regions_ = ids;
    }

    // One poll: GET /api/v3/alerts, stream-parse into the builder.
    // The builder is only committed by the caller on Kind::Ok (SPEC 168).
    Result poll(airalert::SnapshotBuilder& builder);

private:
    Result::Kind parseBody(Stream& in, airalert::SnapshotBuilder& builder, Result& r);

public:

private:
    String token_;
    String lastModified_;
    String mockUrl_;
    String regions_;
    airalert::ApiSnapshotCache cache_;
    BearSSL::Session session_; // TLS resumption between 15 s polls
};
