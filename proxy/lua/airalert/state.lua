-- Pure logic of the alerts proxy: no ngx.* here, so it runs under plain `resty`.
local cjson = require("cjson.safe").new()
cjson.decode_array_with_array_mt(true) -- "[]" stays an array on re-encode
cjson.encode_empty_table_as_object(false)
cjson.encode_escape_forward_slash(false)

local _M = { cjson = cjson }

local MAX_IDS = 32

-- GET /api/v3/alerts body -> array of AlertRegionModel, or nil on anything else.
function _M.decode_alerts(body)
    local doc = cjson.decode(body or "")
    if type(doc) ~= "table" or getmetatable(doc) ~= cjson.array_mt then return nil end
    return doc
end

-- GET /api/v3/regions body -> { [regionId] = parentId | false },
-- { [regionId] = { name =, type = } } (for entries a webhook creates).
function _M.parents(body)
    local doc = cjson.decode(body or "")
    if type(doc) ~= "table" or type(doc.states) ~= "table" then return nil end
    local p, info, n = {}, {}, 0
    local function walk(nodes, parent)
        for _, node in ipairs(nodes) do
            if type(node) == "table" and node.regionId ~= nil and node.regionId ~= cjson.null then
                local id = tostring(node.regionId)
                p[id], n = parent or false, n + 1
                info[id] = { name = node.regionName, type = node.regionType }
                if type(node.regionChildIds) == "table" then walk(node.regionChildIds, id) end
            end
        end
    end
    walk(doc.states, nil)
    if n == 0 then return nil end
    return p, info
end

local function str(v) return type(v) == "string" and v or "" end

-- Webhook event -> the snapshot it describes. `activeAlertLevels` is the FULL
-- state of (regionId, alarmType) after the change: empty = that alert is over.
-- Rewrites every copy of it (an entry also repeats its parents' alerts), drops
-- entries left with nothing, adds the region's own entry when it is new.
-- Returns entries, or nil when the event is unusable.
function _M.apply_event(entries, ev, info)
    if type(ev) ~= "table" or (type(ev.regionId) ~= "number" and type(ev.regionId) ~= "string")
            or type(ev.alarmType) ~= "string" or type(ev.activeAlertLevels) ~= "table" then
        return nil
    end
    local id, t = tostring(ev.regionId), ev.alarmType
    if not id:match("^%d+$") then return nil end
    local levels = setmetatable(ev.activeAlertLevels, cjson.array_mt)
    local active = #levels > 0
    local own -- this region's own entry
    for ei = #entries, 1, -1 do
        local e = entries[ei]
        local list = type(e.activeAlerts) == "table" and e.activeAlerts or {}
        for ai = #list, 1, -1 do
            local a = list[ai]
            if tostring(a.regionId) == id and a.type == t then
                if active then a.activeAlertLevels = levels else table.remove(list, ai) end
            end
        end
        if tostring(e.regionId) == id then own = e end
        if #list == 0 then
            table.remove(entries, ei)
            if own == e then own = nil end
        end
    end
    if not active then return entries end
    local meta = info and info[id] or {}
    if not own then
        own = { regionId = id, regionType = str(meta.type), regionName = str(meta.name),
                regionEngName = "", lastUpdate = str(ev.createdAt),
                activeAlerts = setmetatable({}, cjson.array_mt) }
        entries[#entries + 1] = own
    end
    for _, a in ipairs(own.activeAlerts) do
        if tostring(a.regionId) == id and a.type == t then return entries end
    end
    own.activeAlerts[#own.activeAlerts + 1] = {
        regionId = id, regionType = str(meta.type), type = t,
        lastUpdate = str(ev.createdAt), activeAlertLevels = levels }
    return entries
end

-- "?regions=14,703" -> { "14", "703" }; nil = no filter; nil, err = bad input.
function _M.parse_ids(q)
    if q == nil or q == "" then return nil end
    local ids = {}
    for id in (q .. ","):gmatch("([^,]*),") do
        if not id:match("^%d+$") or #ids >= MAX_IDS then return nil, "bad regions" end
        ids[#ids + 1] = id
    end
    return ids
end

-- Entries a device with these locations needs: the location itself, its
-- ancestors (alert covers it, SPEC 17) and its descendants (partial, SPEC 18).
-- Without a catalog we cannot tell, so everything goes (safe superset).
function _M.select(entries, ids, parents)
    if not ids or not parents then return entries end
    local want, anc = {}, {}
    for _, id in ipairs(ids) do
        want[id] = true
        local p = parents[id]
        while p do anc[p] = true; p = parents[p] end
    end
    local out = setmetatable({}, cjson.array_mt)
    for _, e in ipairs(entries) do
        local id = tostring(e.regionId)
        local hit = anc[id]
        while not hit and id do hit = want[id]; id = parents[id] end
        if hit then out[#out + 1] = e end
    end
    return out
end

-- HTTP dates have 1 s resolution: two changes within one second must still
-- yield different Last-Modified, or a device gets a false 304.
function _M.next_stamp(prev, now)
    return math.max(math.floor(now), (prev or 0) + 1)
end

return _M
