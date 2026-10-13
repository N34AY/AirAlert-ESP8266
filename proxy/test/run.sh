#!/usr/bin/env bash
# Proxy tests: Lua unit tests + end-to-end against tools/alerts_emulator.py.
# Needs docker and python3; uses host networking, ports 18080 (proxy) and 18787 (emulator).
set -euo pipefail
cd "$(dirname "$0")/../.."
IMG=openresty/openresty:1.27.1.2-alpine-fat
P=http://127.0.0.1:18080
W=http://127.0.0.1:18081/webhook/ukrainealarm
E=http://127.0.0.1:18787

docker run --rm -v "$PWD/proxy:/proxy:ro" $IMG resty -I /proxy/lua /proxy/test/test_state.lua

T=$(mktemp -d)
cleanup() { docker rm -f airalert-proxy-test >/dev/null 2>&1 || true; kill "${EMU:-}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

echo test-upstream-key > "$T/ukrainealarm.key"
printf 'dev-token-1  # bench board\n# disabled-token\n' > "$T/devices.txt"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$T/wh.key" 2>/dev/null
openssl pkey -in "$T/wh.key" -pubout -out "$T/webhook-public.pem"
sed -e 's#/opt/airalert-proxy/lua#/proxy/lua#' \
    -e 's#/etc/airalert/#/t/#g' \
    -e 's#listen 8080#listen 18080#' \
    -e 's#listen 8081#listen 18081#' \
    -e '/regions_file/d' \
    -e 's#access_log /var/log/nginx/airalert-webhook.log#access_log off#' \
    -e 's#rate=12r/m#rate=100r/s#' \
    -e 's#access_log /var/log/nginx/airalert-proxy.log#access_log off#' \
    -e "s#-- \"token  \# device\" per line#upstream = \"$E\", poll_every = 1, stale_after = 4, max_per_min = 1000,#" \
    proxy/nginx/airalert-proxy.conf > "$T/airalert.conf"
cat > "$T/nginx.conf" <<'EOF'
worker_processes 2;
error_log stderr warn;
pid /tmp/nginx.pid;
events {}
http { include /t/airalert.conf; }
EOF

python3 tools/alerts_emulator.py --port 18787 --require-token >"$T/emu.log" 2>&1 </dev/null &
EMU=$!
docker run -d --name airalert-proxy-test --network host -v "$PWD/proxy:/proxy:ro" -v "$T:/t:ro" $IMG \
    sh -c 'opm get ledgetech/lua-resty-http fffonion/lua-resty-openssl >/dev/null 2>&1 && openresty -c /t/nginx.conf -g "daemon off;"' >/dev/null

fail() { echo "FAIL: $*"; docker logs airalert-proxy-test 2>&1 | tail -20; exit 1; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
get() { curl -s -H 'Authorization: dev-token-1' "$@"; }
ctl() { curl -s "$E/ctl?cmd=$1" >/dev/null; }

for _ in $(seq 60); do curl -s "$P/health" 2>/dev/null | grep -q '"fresh":true' && break; sleep 1; done
curl -s "$P/health" | grep -q '"fresh":true' || fail "proxy never got fresh data"
sleep 2 # one more tick: catalog

[ "$(code "$P/api/v3/alerts")" = 403 ] || fail "no auth must be 403"
[ "$(code -H 'Authorization: nope' "$P/api/v3/alerts")" = 401 ] || fail "unknown token must be 401"
[ "$(code -H 'Authorization: disabled-token' "$P/api/v3/alerts")" = 401 ] || fail "commented token must be 401"
[ "$(code -H 'Authorization: dev-token-1' "$P/api/v3/alerts?regions=1,x")" = 400 ] || fail "bad regions must be 400"
[ "$(get "$P/api/v3/alerts")" = "[]" ] || fail "quiet country must be []"

ctl 'start+air_raid+14'; ctl 'start+air_raid+31'; sleep 2.5
all=$(get "$P/api/v3/alerts")
echo "$all" | grep -q '"regionId": *"14"' && echo "$all" | grep -q '"regionId": *"31"' || fail "full list: $all"
echo "$all" | grep -q 'Київська область' || fail "unicode lost: $all"
mine=$(get "$P/api/v3/alerts?regions=703")
echo "$mine" | grep -q '"regionId": *"14"' || fail "703 must get oblast 14: $mine"
echo "$mine" | grep -q '"regionId": *"31"' && fail "703 must not get Kyiv city: $mine"
[ "$(get "$P/api/v3/alerts?regions=16")" = "[]" ] || fail "16 must get []"

lm=$(curl -s -D - -o /dev/null -H 'Authorization: dev-token-1' "$P/api/v3/alerts?regions=703" | tr -d '\r' | sed -n 's/^Last-Modified: //p')
[ -n "$lm" ] || fail "no Last-Modified"
[ "$(code -H 'Authorization: dev-token-1' -H "If-Modified-Since: $lm" "$P/api/v3/alerts?regions=703")" = 304 ] || fail "same state must be 304"
ctl 'stop+air_raid+14'; sleep 2.5
[ "$(code -H 'Authorization: dev-token-1' -H "If-Modified-Since: $lm" "$P/api/v3/alerts?regions=703")" = 200 ] || fail "changed state must be 200"
[ "$(get "$P/api/v3/alerts?regions=703")" = "[]" ] || fail "all-clear for 703 must be []"

# webhook: signed "{ts}.{body}"; hold the snapshot (upstream down) so only webhooks change state
hook() { # body [ts] [key]
    local ts=${2:-$(date +%s)} sig
    sig=$(printf '%s.%s' "$ts" "$1" | openssl dgst -sha256 -sign "${3:-$T/wh.key}" | base64 -w0)
    curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Webhook-Signature: $sig" \
        -H "X-Webhook-Timestamp: $ts" -H 'Content-Type: application/json' --data-binary "$1" "$W"
}
ev() { echo "{\"status\":\"$1\",\"regionId\":$2,\"alarmType\":\"AIR\",\"createdAt\":\"2026-10-13T10:00:00Z\",\"activeAlertLevels\":$3}"; }
red='[{"alertLevel":"Red","reason":"x","createdAt":"2026-10-13T10:00:00Z"}]'
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$T/evil.key" 2>/dev/null
[ "$(code "$P/webhook/ukrainealarm")" = 404 ] || fail "webhook must not be on the device port"
[ "$(code "$W")" = 405 ] || fail "GET webhook must be 405"
ctl 'mode+500'
[ "$(hook "$(ev ACTIVATE 75 "$red")" "" "$T/evil.key")" = 401 ] || fail "foreign signature must be 401"
[ "$(hook "$(ev ACTIVATE 75 "$red")" $(( $(date +%s) - 600 )))" = 200 ] || fail "stale webhook must be acked"
[ "$(get "$P/api/v3/alerts?regions=703")" = "[]" ] || fail "stale/forged webhook must not apply"
e=$(ev ACTIVATE 75 "$red"); ts=$(date +%s)
[ "$(hook "$e" "$ts")" = 200 ] || fail "good webhook must be 200"
[ "$(hook "$e" "$ts")" = 200 ] || fail "replay must be acked"
mine=$(get "$P/api/v3/alerts?regions=703")
echo "$mine" | grep -q '"regionId":"75"' && echo "$mine" | grep -q 'Бучанський' || fail "webhook start for raion 75: $mine"
[ "$(hook "$(ev DEACTIVATE 75 '[]')")" = 200 ] || fail "end webhook"
[ "$(get "$P/api/v3/alerts?regions=703")" = "[]" ] || fail "webhook end for 75"
[ "$(hook "$(ev ACTIVATE 16 "$red")")" = 200 ] || fail "start 16"
get "$P/api/v3/alerts?regions=16" | grep -q '"regionId":"16"' || fail "webhook start for 16"
h=$(curl -s "$P/health"); echo "$h" | grep -q '"ok":3' && echo "$h" | grep -q '"dup":1' && echo "$h" | grep -q '"stale":1' \
    && echo "$h" | grep -q '"bad":1' || fail "webhook counters: $h"
ctl 'mode+ok'; sleep 2.5
[ "$(get "$P/api/v3/alerts?regions=16")" = "[]" ] || fail "snapshot must reconcile a webhook the source never confirmed"

ctl 'mode+500'; sleep 6
[ "$(code -H 'Authorization: dev-token-1' "$P/api/v3/alerts")" = 503 ] || fail "stale data must be 503"
curl -s "$P/health" | grep -q '"fresh":false' || fail "health must say stale"
ctl 'mode+ok'; sleep 2.5
[ "$(code -H 'Authorization: dev-token-1' "$P/api/v3/alerts")" = 200 ] || fail "must recover after upstream is back"

echo "e2e: OK  $(curl -s "$P/health")"
