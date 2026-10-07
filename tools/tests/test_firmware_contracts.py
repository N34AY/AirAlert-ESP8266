import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class FirmwareBoundaryContractTests(unittest.TestCase):
    def test_http_validator_is_committed_only_after_a_valid_snapshot(self):
        source = (ROOT / "src/alerts/AlertsClient.cpp").read_text()

        # streaming per-element parser replaced whole-document extractAlerts
        # (constant memory vs nationwide alert count); the contract is the
        # same: the cache validator commits only after a fully valid body
        parsed = source.index("parseBody(http.getStream()")
        committed = source.index("lastModified_ = responseLastModified")
        self.assertLess(parsed, committed)
        self.assertIn("buildAlertElementFilter", source)
        self.assertIn("cache_.commitValidSnapshot()", source[committed:])
        self.assertNotIn("new BearSSL::WiFiClientSecure", source)
        self.assertNotIn("setInsecure", source)

    def test_not_modified_response_reapplies_the_cached_confirmation_sample(self):
        source = (ROOT / "src/main.cpp").read_text()
        start = source.index("case Kind::NotModified:")
        end = source.index("case Kind::AuthError:", start)

        self.assertIn("applyAndNotify(builder.snapshot(), false)", source[start:end])

    def test_location_refresh_invalidates_old_state_and_validator(self):
        source = (ROOT / "src/main.cpp").read_text()
        start = source.index("static void refreshLocations()")
        end = source.index("static void applyConfig()", start)
        body = source[start:end]

        self.assertIn("engine.reset()", body)
        self.assertIn("builder.reset()", body)
        self.assertIn("client.invalidateCache()", body)
        self.assertIn("nextPollAt = millis()", body)

    def test_failed_ota_resumes_the_main_loop(self):
        web = (ROOT / "src/web/WebUi.cpp").read_text()
        main = (ROOT / "src/main.cpp").read_text()

        self.assertIn("d_.finishFailedOta()", web)
        self.assertIn("otaInProgress = false", main)

    def test_failed_parse_drops_the_validator_so_no_304_can_reuse_it(self):
        source = (ROOT / "src/alerts/AlertsClient.cpp").read_text()
        start = source.index("case HTTP_CODE_OK:")
        end = source.index("case HTTP_CODE_NOT_MODIFIED:", start)
        body = source[start:end]

        # the builder is reset before streaming, so a mid-body failure leaves a
        # partial snapshot: it must never be reachable through a later 304
        self.assertIn("invalidateCache()", body)
        self.assertLess(body.index("invalidateCache()"), body.index("lastModified_ ="))

    def test_relay_begin_clears_the_cached_on_state(self):
        source = (ROOT / "src/hardware/RelayController.h").read_text()
        begin = source.index("void begin(")
        end = source.index("void tick(", begin)

        # begin() re-runs on every config save; a stale on_ would make the next
        # apply(true) a no-op - relay dead and safety deadline never armed
        self.assertIn("on_ = false;", source[begin:end])

    def test_ota_cannot_wedge_the_device_forever(self):
        source = (ROOT / "src/main.cpp").read_text()
        prepare = source.index("static void prepareOta()")
        finish = source.index("static void refreshLocations()")
        body = source[prepare:finish]

        # loop() is blocked while an upload stalls, so the deadline must be a
        # SYS-context timer, like the relay safety timer
        self.assertIn("otaWatchdog.once_ms", body)
        self.assertIn("ESP.restart()", body)
        self.assertIn("otaWatchdog.detach()", body)

    def test_wifi_scan_is_open_only_on_the_provisioning_portal(self):
        source = (ROOT / "src/web/WebUi.cpp").read_text()
        start = source.index("void WebUi::handleScan()")
        end = source.index("WiFi.scanNetworks()", start)

        self.assertIn("WifiService::State::Provisioning", source[start:end])
        self.assertIn("!authed()", source[start:end])

    def test_poll_never_starts_while_the_relay_is_on(self):
        source = (ROOT / "src/main.cpp").read_text()
        loop = source.index("void loop()")
        call = source.index("doPoll();", loop)
        line = source[source.rindex("\n", 0, call):call]

        # the poll blocks loop() for seconds: it must wait for the OFF gap
        self.assertIn("!relay.isOn()", line)

    def test_relay_deadline_uses_an_independent_timer_callback(self):
        source = (ROOT / "src/hardware/RelayController.h").read_text()

        self.assertIn("safetyTimer_.once_ms", source)
        self.assertIn("void emergencyOff()", source)
        body = source[source.index("void emergencyOff()"):source.index("void apply(")]
        self.assertIn("pulse(stopPin_, ", body)

    def test_siren_is_driven_by_start_and_stop_pulses(self):
        source = (ROOT / "src/hardware/RelayController.h").read_text()
        apply = source[source.index("void apply("):source.index("void pulse(")]

        # latching siren: a state change is a timed button press, never a level
        self.assertIn("pulse(on ? startPin_ : stopPin_, ", apply)
        self.assertIn("pulseTimer_.once_ms", source)
        # a config save mid-pattern must stop a latched siren
        begin = source[source.index("void begin("):source.index("void tick(")]
        self.assertIn("if (wasOn) pulse(stopPin_, ", begin)

    def test_single_relay_test_cannot_leave_the_siren_unbounded(self):
        source = (ROOT / "src/hardware/RelayController.h").read_text()
        body = source[source.index("void testPulse("):source.index("bool isOn()")]
        web = (ROOT / "src/web/WebUi.cpp").read_text()
        route = web[web.index('"/api/v1/test/relay"'):web.index('"/api/v1/relay/off"')]

        # a hand-pressed START still gets the safety deadline
        self.assertIn("safetyTimer_.once_ms", body)
        # never interleave hand presses with a playing pattern
        self.assertIn("d_.notify->playing()", route)
        self.assertLess(route.index("playing()"), route.index("testPulse("))

    def test_every_relay_press_reaches_the_event_log(self):
        relay = (ROOT / "src/hardware/RelayController.h").read_text()
        source = (ROOT / "src/main.cpp").read_text()
        pulse = relay[relay.index("void pulse(uint8_t pin"):relay.index("void endPulse()")]
        loop = source[source.index("void loop()"):]

        self.assertIn("queuePress(", pulse)
        self.assertIn("logRelayPresses();", loop)
        self.assertIn("LogEvent::RelayPress", source)

    def test_boot_never_touches_siren_relays(self):
        source = (ROOT / "src/main.cpp").read_text()
        setup = source[source.index("void setup()"):source.index("void loop()")]

        # no STOP press on power-up, and no pin drive before the real polarity
        # is known (applyConfig -> relay.begin)
        self.assertNotIn("relay.forceOff();", setup)
        self.assertNotIn("relay.begin(", setup)


if __name__ == "__main__":
    unittest.main()
