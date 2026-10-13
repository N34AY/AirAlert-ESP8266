-- nginx glue: one worker refreshes ukrainealarm into the shared dict, every
-- worker serves devices from it. State lives only in memory (lost on restart,
-- kept across reload) and is rebuilt from upstream.
local http = require "resty.http"
local pkey = require "resty.openssl.pkey"
local state = require "airalert.state"

local cjson = state.cjson
local dict = ngx.shared.airalert
local _M = {}

local cfg = {
    upstream = "https://api.ukrainealarm.com",
    poll_every = 20,       -- s; ~3 accepted req/min per key (docs/RESEARCH.md)
    regions_every = 86400, -- s; catalog barely changes
    regions_retry = 3600,  -- s between failed catalog attempts: /regions often 401s, alerts matter more
    stale_after = 180,     -- s without a good snapshot -> 503 to devices
    max_per_min = 3,       -- hard cap on upstream requests, shared by all calls
    max_skew = 300,        -- s; older (or future) webhooks are dropped, as ukrainealarm advises
}

local function read_file(path)
    local f = assert(io.open(path, "rb"))
    local s = f:read("*a")
    f:close()
    return s
end

-- init_by_lua: secrets are read once per (re)load, never from devices.
function _M.configure(c)
    for k, v in pairs(c) do cfg[k] = v end
    cfg.key = read_file(cfg.key_file):match("^%s*(.-)%s*$")
    cfg.tokens = {}
    for line in read_file(cfg.tokens_file):gmatch("[^\r\n]+") do
        local t = line:match("^%s*([^#%s]+)") -- "token  # device name"
        if t then cfg.tokens[t] = true end
    end
    -- seed catalog (proxy/nginx/regions.json): filtering works before /regions ever answers
    if cfg.regions_file then
        cfg.regions_seed = read_file(cfg.regions_file)
        assert(state.parents(cfg.regions_seed), "bad regions_file")
    end
    if cfg.webhook_pem_file then
        cfg.webhook_key = assert(pkey.new(read_file(cfg.webhook_pem_file)))
    end
end

local function count(key) dict:incr(key, 1, 0) end

-- One line per upstream request (only worker 0 polls, so no interleaving).
-- ponytail: blocking append of ~3 lines/min; logrotate with copytruncate
local function trace(path, status, ms, bytes)
    if not cfg.upstream_log then return end
    local f = io.open(cfg.upstream_log, "a")
    if not f then return end
    f:write(ngx.localtime(), " ", path, " ", status, " ", ms, "ms ", bytes, "B\n")
    f:close()
end

local function fetch(path)
    local used = dict:incr("budget", 1, 0, 60) -- fixed 60 s window
    if used > cfg.max_per_min then return nil, "budget" end
    count("up_req")
    ngx.update_time()
    local t0 = ngx.now()
    local res, err = http.new():request_uri(cfg.upstream .. path, {
        headers = { ["Authorization"] = cfg.key, ["User-Agent"] = "airalert-proxy/1" },
        ssl_verify = true,
    })
    ngx.update_time()
    trace(path, res and res.status or tostring(err), math.floor((ngx.now() - t0) * 1000),
          res and #(res.body or "") or 0)
    if not res then return nil, err end
    -- 401 is usually the rate limit, not a bad key: just try next tick
    if res.status ~= 200 then return nil, "http " .. res.status end
    return res.body
end

local function fail(what, err)
    count("up_fail")
    dict:set("last_error", what .. ": " .. tostring(err))
    ngx.log(ngx.WARN, "airalert upstream ", what, ": ", err)
end

-- Snapshot and webhooks both rewrite "alerts" from any worker: one writer at a time.
-- ponytail: spin lock on the same dict; resty.lock if it ever shows up in latency
local function locked(fn, ...)
    for _ = 1, 200 do
        if dict:add("lock", true, 5) then
            local ok, err = pcall(fn, ...)
            dict:delete("lock")
            if not ok then error(err) end
            return
        end
        ngx.sleep(0.01)
    end
    error("lock timeout")
end

local function store(body, now)
    if body ~= dict:get("alerts") then
        dict:set("alerts", body)
        dict:set("changed_at", state.next_stamp(dict:get("changed_at"), now))
    end
end

local function refresh()
    local now = ngx.now()
    -- catalog only once alerts are fresh: alerts are what keeps devices alive
    if now - (dict:get("regions_at") or 0) > cfg.regions_every
            and now - (dict:get("regions_try") or 0) > cfg.regions_retry
            and now - (dict:get("alerts_at") or 0) < cfg.poll_every * 2 then
        dict:set("regions_try", now)
        local body, err = fetch("/api/v3/regions")
        if body and state.parents(body) then
            dict:set("regions", body)
            dict:set("regions_at", state.next_stamp(dict:get("regions_at"), now))
        else
            fail("regions", err or "bad body")
        end
        return
    end
    local body, err = fetch("/api/v3/alerts")
    if not body or not state.decode_alerts(body) then return fail("alerts", err or "bad body") end
    -- the snapshot wins over webhooks applied meanwhile: it is the reconciliation
    locked(store, body, now)
    dict:set("alerts_at", now)
    count("up_ok")
end

function _M.start()
    if ngx.worker.id() ~= 0 then return end
    if cfg.regions_seed and not dict:get("regions") then
        dict:set("regions", cfg.regions_seed)
        dict:set("regions_at", ngx.now()) -- next upstream try in regions_every
    end
    local function tick(premature)
        if premature then return end
        local ok, err = pcall(refresh)
        if not ok then fail("tick", err) end
    end
    ngx.timer.at(0, tick)
    ngx.timer.every(cfg.poll_every, tick)
end

-- Per-worker decoded copies, invalidated by the dict timestamps.
local cache = {}
local function decoded(name, at_key, parse)
    local at = dict:get(at_key)
    local c = cache[name]
    if not c or c.at ~= at then
        c = { at = at, v = parse(dict:get(name)) }
        cache[name] = c
    end
    return c.v
end

function _M.serve_alerts()
    local auth = ngx.var.http_authorization
    if not auth then return ngx.exit(403) end              -- same codes as upstream
    if not cfg.tokens[auth] then return ngx.exit(401) end
    local ok_at = dict:get("alerts_at")
    if not ok_at or ngx.now() - ok_at > cfg.stale_after then
        -- never hand out an old picture as fresh: the device must see API_OFFLINE
        ngx.header["Retry-After"] = cfg.poll_every
        return ngx.exit(503)
    end
    local ids, err = state.parse_ids(ngx.var.arg_regions)
    if err then return ngx.exit(400) end

    local parents = ids and decoded("regions", "regions_at", state.parents)
    -- the catalog arriving turns a full list into a filtered one: new validator
    local stamp = dict:get("changed_at")
    if parents then stamp = math.max(stamp, dict:get("regions_at")) end
    local lm = ngx.http_time(stamp)
    ngx.header["Last-Modified"] = lm
    if ngx.var.http_if_modified_since == lm then return ngx.exit(304) end

    local body = dict:get("alerts")
    if parents then
        body = cjson.encode(state.select(decoded("alerts", "changed_at", state.decode_alerts), ids, parents))
    end
    ngx.header["Content-Type"] = "application/json; charset=utf-8"
    ngx.print(body)
end

-- POST from ukrainealarm (`POST /api/v3/webhook {webHookUrl}`): RSA-SHA256 over
-- "{timestamp}.{raw body}". Answers 200 to anything signed, so they never retry
-- a message we chose to drop; the next snapshot covers it anyway.
function _M.serve_webhook()
    if ngx.req.get_method() ~= "POST" then return ngx.exit(405) end
    if not cfg.webhook_key then return ngx.exit(404) end
    local sig = ngx.var.http_x_webhook_signature
    local ts = tonumber(ngx.var.http_x_webhook_timestamp)
    ngx.req.read_body()
    local body = ngx.req.get_body_data()
    if not sig or not ts or not body then count("wh_bad"); return ngx.exit(400) end
    local raw = ngx.decode_base64(sig)
    if not raw or not cfg.webhook_key:verify(raw, ts .. "." .. body, "sha256") then
        count("wh_bad")
        ngx.log(ngx.WARN, "airalert webhook: bad signature")
        return ngx.exit(401)
    end
    if math.abs(ngx.now() - ts) > cfg.max_skew then count("wh_stale"); return ngx.exit(200) end
    if not dict:add("wh:" .. ngx.md5(sig), true, cfg.max_skew * 2) then
        count("wh_dup"); return ngx.exit(200)
    end
    local ev = cjson.decode(body)
    local applied
    locked(function()
        local entries = state.decode_alerts(dict:get("alerts"))
        if not entries then return end -- no snapshot yet: it will carry this event
        local _, info = state.parents(dict:get("regions"))
        entries = state.apply_event(entries, ev, info)
        if entries then
            store(cjson.encode(entries), ngx.now())
            applied = true
        end
    end)
    count(applied and "wh_ok" or "wh_skip")
    dict:set("webhook_at", ngx.now())
    ngx.log(ngx.INFO, "airalert webhook ", body)
    ngx.exit(200)
end

function _M.serve_health()
    local now = ngx.now()
    local function age(k) local t = dict:get(k); return t and math.floor(now - t) or cjson.null end
    local devices = 0
    for _ in pairs(cfg.tokens) do devices = devices + 1 end
    ngx.header["Content-Type"] = "application/json"
    ngx.say(cjson.encode({
        fresh = (dict:get("alerts_at") or 0) > now - cfg.stale_after,
        alerts_age_s = age("alerts_at"),
        changed_age_s = age("changed_at"),
        regions_age_s = age("regions_at"),
        upstream_requests = dict:get("up_req") or 0,
        upstream_ok = dict:get("up_ok") or 0,
        upstream_fail = dict:get("up_fail") or 0,
        last_error = dict:get("last_error") or cjson.null,
        webhook_age_s = age("webhook_at"),
        webhooks = { ok = dict:get("wh_ok") or 0, skip = dict:get("wh_skip") or 0,
                     dup = dict:get("wh_dup") or 0, stale = dict:get("wh_stale") or 0,
                     bad = dict:get("wh_bad") or 0 },
        devices = devices,
    }))
end

return _M
