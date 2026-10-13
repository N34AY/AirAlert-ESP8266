-- resty -I proxy/lua proxy/test/test_state.lua   (run via proxy/test/run.sh)
local state = require "airalert.state"
local cjson = state.cjson

local regions = [[{"states":[
 {"regionId":"14","regionName":"Київська область","regionType":"State","regionChildIds":[
  {"regionId":"75","regionName":"Бучанський район","regionType":"District","regionChildIds":[
   {"regionId":"703","regionName":"Ірпінська","regionType":"Community","regionChildIds":[]},
   {"regionId":"704","regionName":"Бучанська","regionType":"Community","regionChildIds":null}]}]},
 {"regionId":"31","regionName":"м. Київ","regionType":"State","regionChildIds":[]}]}]]

local p = assert(state.parents(regions))
assert(p["14"] == false and p["75"] == "14" and p["703"] == "75" and p["704"] == "75")
assert(state.parents('{"states":[]}') == nil and state.parents("junk") == nil)

-- ids
assert(state.parse_ids(nil) == nil and state.parse_ids("") == nil)
local ids = state.parse_ids("703,31")
assert(#ids == 2 and ids[1] == "703" and ids[2] == "31")
for _, bad in ipairs({ "1,,2", "1,x", "-1", "1;2", "14," }) do
    local v, err = state.parse_ids(bad)
    assert(v == nil and err, "should reject " .. bad)
end
assert(select(2, state.parse_ids(("1,"):rep(33):sub(1, -2))) == "bad regions")

-- alerts body must be an array
assert(state.decode_alerts("{}") == nil and state.decode_alerts("") == nil)
local empty = assert(state.decode_alerts("[]"))
assert(cjson.encode(empty) == "[]")

local entry = function(id) return { regionId = id, activeAlerts = setmetatable({}, cjson.array_mt) } end
local all = { entry("14"), entry("75"), entry("703"), entry("704"), entry("31"), entry("16"), entry("999") }
local function got(list)
    local s = {}
    for _, e in ipairs(list) do s[#s + 1] = e.regionId end
    return table.concat(s, ",")
end

-- hromada 703: itself + ancestors 75, 14; not its sibling 704 or other oblasts
assert(got(state.select(all, { "703" }, p)) == "14,75,703")
-- raion 75: ancestor 14 + descendants 703, 704 (partial coverage, SPEC 18)
assert(got(state.select(all, { "75" }, p)) == "14,75,703,704")
-- several locations, unknown one only matches itself
assert(got(state.select(all, { "31", "999" }, p)) == "31,999")
-- nothing relevant -> empty JSON array, not {}
assert(cjson.encode(state.select(all, { "16000" }, p)) == "[]")
-- no catalog / no filter -> full list (safe superset)
assert(#state.select(all, { "703" }, nil) == #all and #state.select(all, nil, p) == #all)

-- format round-trip keeps unicode, slashes and empty arrays as-is
local body = '[{"regionId":"14","regionName":"Київська область","activeAlerts":[],"reason":"a/b"}]'
local out = cjson.encode(state.decode_alerts(body))
assert(out:find("Київська", 1, true) and out:find('"activeAlerts":[]', 1, true) and out:find("a/b", 1, true), out)

-- validators strictly increase even within one second
assert(state.next_stamp(nil, 100.7) == 100)
assert(state.next_stamp(100, 100.2) == 101 and state.next_stamp(101, 250.0) == 250)

-- webhooks: activeAlertLevels is the full state of (region, type)
local _, info = state.parents(regions)
assert(info["703"].name == "Ірпінська" and info["703"].type == "Community")
local lvl = function(l) return { { alertLevel = l, createdAt = "2026-10-13T10:00:00Z" } } end
local snap = function() return assert(state.decode_alerts([[ [
 {"regionId":"14","regionType":"State","activeAlerts":[{"regionId":"14","type":"AIR","activeAlertLevels":[{"alertLevel":"Red"}]}]},
 {"regionId":"703","regionType":"Community","activeAlerts":[
   {"regionId":"14","type":"AIR","activeAlertLevels":[{"alertLevel":"Red"}]},
   {"regionId":"703","type":"ARTILLERY","activeAlertLevels":[]}]}] ]])) end
local ev = function(id, t, levels) return { regionId = id, alarmType = t, activeAlertLevels = levels } end

-- end of oblast alert: own entry gone, inherited copy in 703 gone, 703 keeps its own
local s1 = state.apply_event(snap(), ev(14, "AIR", setmetatable({}, cjson.array_mt)), info)
assert(got(s1) == "703" and #s1[1].activeAlerts == 1 and s1[1].activeAlerts[1].type == "ARTILLERY")
assert(cjson.encode(s1):find('"activeAlertLevels":[]', 1, true)) -- untouched empty array stays []
-- level change rewrites every copy
local s2 = state.apply_event(snap(), ev(14, "AIR", lvl("Yellow")), info)
assert(s2[1].activeAlerts[1].activeAlertLevels[1].alertLevel == "Yellow"
   and s2[2].activeAlerts[1].activeAlertLevels[1].alertLevel == "Yellow")
-- new region gets an entry with catalog name/type; second identical event changes nothing
local s3 = state.apply_event(snap(), ev(75, "AIR", lvl("Red")), info)
assert(got(s3) == "14,703,75" and s3[3].regionType == "District" and s3[3].activeAlerts[1].regionId == "75")
assert(#state.apply_event(s3, ev("75", "AIR", lvl("Red")), info)[3].activeAlerts == 1)
-- new type on an existing entry; unknown region without catalog still works
local s4 = state.apply_event(snap(), ev(14, "CHEMICAL", lvl("Red")), nil)
assert(#s4[1].activeAlerts == 2 and s4[1].activeAlerts[2].type == "CHEMICAL")
assert(got(state.apply_event(snap(), ev(9999, "AIR", lvl("Red")), nil)) == "14,703,9999")
-- end for a region not in the snapshot: no-op; junk events rejected
assert(got(state.apply_event(snap(), ev(31, "AIR", {}), info)) == "14,703")
assert(state.apply_event(snap(), { regionId = 14 }, info) == nil)
assert(state.apply_event(snap(), ev("1;2", "AIR", {}), info) == nil)

print("test_state: OK")
