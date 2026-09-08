-- Bot Inspector -- transport and assembly.
--
-- Everything shown by this addon was sent by the server: a WoW addon cannot query anything itself.
-- The server half lives in mod-playerbots (BotInspectorMgr) and speaks the protocol below.
--
-- 3.3.5a notes that differ from modern WoW, and that cost real time to establish:
--   * RegisterAddonMessagePrefix does not exist here -- it arrived in 4.0. Every addon message is
--     delivered to every addon, so filtering on our own prefix in Lua is the whole mechanism. Do not
--     add a registration call; it will simply error.
--   * CHAT_MSG_ADDON delivers the prefix as arg1 and the body as arg2, the client having split on
--     the first tab.
--   * Addon messages cap out near 255 bytes, hence the chunking envelope.

BotInspector = BotInspector or {}
local BI = BotInspector

BI.PREFIX  = "PBI"
BI.VERSION = 1

-- Assembly buffers, keyed by "verb:key". A response is complete when seq == total.
local pending  = {}
local handlers = {}

-- Forward declarations.
--
-- Lua binds a `local function` name only from its definition onward. A local defined further down
-- this file is therefore a *global* lookup -- and so nil -- inside any function written above it.
-- dispatch() calls clearInflight(), which used to be declared near the watchdog at the bottom: the
-- call compiled to a nil global and every reply died on dispatch's first line. From the outside
-- that was indistinguishable from the server never answering, because the watchdog then reported a
-- timeout. luac sees nothing wrong with it; only running it can find it. Declare here, define below.
local inflight = {}
local clearInflight

BI.zones   = {}   -- zoneId -> bot count
BI.roster  = {}   -- zoneId -> { {guid, name, level, class, race, gold}, ... }
BI.detail  = {}   -- guid -> { CORE = {...}, GEAR = {...}, ... }
BI.lastError = nil

local function split(str, sep)
    local out = {}
    for piece in string.gmatch(str, "([^" .. sep .. "]+)") do
        table.insert(out, piece)
    end
    return out
end
BI.split = split

--- Send one request to the server.
-- Whispering ourselves is the standard 3.3.5 route for addon-to-server traffic: there is no
-- server-directed channel, and the server consumes the message before it reaches anyone.
function BI:Send(...)
    local parts = { self.VERSION, ... }
    local msg = table.concat(parts, "\t")
    SendAddonMessage(self.PREFIX, msg, "WHISPER", UnitName("player"))
end

function BI:RequestZones()
    self:Send("ZONES"); self:MarkSent("ZONES", "0")
end
function BI:RequestList(zoneId)
    self:Send("LIST", zoneId); self:MarkSent("LIST", tostring(zoneId))
end
function BI:RequestFind(needle)
    self:Send("FIND", needle); self:MarkSent("FIND", needle)
end
function BI:RequestDetail(guid, section)
    self:Send("DETAIL", guid, section); self:MarkSent("DETAIL", guid .. ":" .. section)
end

--- Called once a multi-chunk response is fully assembled.
local function dispatch(verb, key, rows)
    clearInflight(verb, key)

    if verb == "ERR" then
        BI.lastError = rows[1] or "unknown error"
        if handlers.OnError then handlers.OnError(key, BI.lastError) end
        return
    end

    if verb == "ZONES" then
        BI.zones = {}
        for _, row in ipairs(rows) do
            local f = split(row, ":")
            if f[1] then BI.zones[tonumber(f[1])] = tonumber(f[2]) or 0 end
        end
        if handlers.OnZones then handlers.OnZones(BI.zones) end

    elseif verb == "LIST" then
        local zoneId = tonumber(key)
        local list = {}
        for _, row in ipairs(rows) do
            local f = split(row, ":")
            if f[1] then
                table.insert(list, {
                    guid = tonumber(f[1]), name = f[2], level = tonumber(f[3]),
                    class = tonumber(f[4]), race = tonumber(f[5]), gold = tonumber(f[6]),
                })
            end
        end
        BI.roster[zoneId] = list
        if handlers.OnList then handlers.OnList(zoneId, list) end

    elseif verb == "FIND" then
        local list, matched = {}, 0
        for _, row in ipairs(rows) do
            local f = split(row, ":")
            if f[1] == "#matched" then
                matched = tonumber(f[2]) or 0
            elseif f[1] then
                table.insert(list, { guid = tonumber(f[1]), name = f[2],
                                     level = tonumber(f[3]), zone = tonumber(f[4]) })
            end
        end
        if handlers.OnFind then handlers.OnFind(list, matched) end

    elseif verb == "DETAIL" then
        local f = split(key, ":")
        local guid, section = tonumber(f[1]), f[2]
        BI.detail[guid] = BI.detail[guid] or {}
        BI.detail[guid][section] = rows
        BI.detail[guid].fetchedAt = GetTime()
        if handlers.OnDetail then handlers.OnDetail(guid, section, rows) end
    end
end

--- Buffer a chunk, and dispatch once the last one lands.
local function receive(body)
    local fields = {}
    for piece in string.gmatch(body .. "\t", "([^\t]*)\t") do
        table.insert(fields, piece)
    end

    -- <version> <verb> <key> <seq> <total> <payload...>
    local version, verb, key = tonumber(fields[1]), fields[2], fields[3]
    local seq, total = tonumber(fields[4]), tonumber(fields[5])

    if version ~= BI.VERSION or not verb or not seq or not total then
        return
    end

    local id = verb .. ":" .. (key or "")
    local buf = pending[id]
    if not buf or seq == 1 then
        buf = { rows = {}, total = total }
        pending[id] = buf
    end

    for i = 6, #fields do
        if fields[i] ~= "" then table.insert(buf.rows, fields[i]) end
    end

    if seq >= total then
        pending[id] = nil

        -- Run dispatch under pcall. An error in an event handler is swallowed by the client unless
        -- scriptErrors is on, so without this a broken parser looks exactly like a dead server --
        -- which is precisely how the clearInflight bug above hid for two rounds of testing.
        local ok, err = pcall(dispatch, verb, key, buf.rows)
        if not ok then
            clearInflight(verb, key)
            BI.lastError = "addon error: " .. tostring(err)
            if handlers.OnError then handlers.OnError(key, BI.lastError) end
        end
    end
end

--- Time out a request that never gets an answer.
--
-- Without this the panel waits forever, which is indistinguishable from a slow reply and hides the
-- most common real cause: the server has no inspector responder, or refused silently. A debugging
-- tool that cannot tell "waiting" from "nobody is listening" is not much of one.
local REQUEST_TIMEOUT = 5

function BI:MarkSent(verb, key)
    inflight[verb .. ":" .. (key or "")] = GetTime()
end

function clearInflight(verb, key)
    inflight[verb .. ":" .. (key or "")] = nil
end

local watchdog = CreateFrame("Frame")
watchdog:SetScript("OnUpdate", function(self, elapsed)
    self.acc = (self.acc or 0) + elapsed
    if self.acc < 1 then return end
    self.acc = 0

    local now = GetTime()
    for id, sentAt in pairs(inflight) do
        if now - sentAt > REQUEST_TIMEOUT then
            inflight[id] = nil
            pending[id] = nil
            if handlers.OnTimeout then handlers.OnTimeout(id) end
        end
    end
end)

function BI:SetHandler(name, fn) handlers[name] = fn end

local frame = CreateFrame("Frame")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:SetScript("OnEvent", function(self, event, prefix, message)
    if prefix == BI.PREFIX then
        receive(message)
    end
end)

SLASH_BOTINSPECTOR1 = "/botinspector"
SLASH_BOTINSPECTOR2 = "/bi"
SlashCmdList["BOTINSPECTOR"] = function(arg)
    if arg and arg ~= "" then
        BI:RequestFind(arg)
        if BotInspectorFrame then BotInspectorFrame:Show() end
    else
        BI:Toggle()
    end
end
