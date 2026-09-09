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

-- Ceiling on rows in a single assembly. See the check in receive().
local MAX_ASSEMBLY_ROWS = 2000

BI.zones     = {}   -- zoneId -> bot count
BI.zoneNames = {}   -- zoneId -> name (server-sent; the client cannot resolve area ids itself)
BI.roster  = {}   -- zoneId -> { {guid, name, level, class, race, gold}, ... }
BI.detail  = {}   -- guid -> { CORE = {...}, GEAR = {...}, ... }
BI.lastError = nil
BI.alts      = {}   -- { {guid, name, level, class, race, state}, ... }
BI.can       = { appear = false, summon = false }   -- filled in by the ZONES reply

-- Preserves empty fields. The obvious "([^:]+)" pattern silently drops them, which shifts every
-- field after a blank one and makes a row parse as something plausible but wrong -- the worst way
-- for a debugging tool to fail. Appending the separator lets the trailing field match too.
local function split(str, sep)
    local out = {}
    for piece in string.gmatch(str .. sep, "([^" .. sep .. "]*)" .. sep) do
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
    self:Send("ZONES")
    self:MarkSent("ZONES", "0", function() BI:RequestZones() end)
end
function BI:RequestList(zoneId)
    self:Send("LIST", zoneId)
    self:MarkSent("LIST", tostring(zoneId), function() BI:RequestList(zoneId) end)
end
function BI:RequestFind(needle)
    self:Send("FIND", needle)
    self:MarkSent("FIND", needle, function() BI:RequestFind(needle) end)
end
function BI:RequestDetail(guid, section)
    self:Send("DETAIL", guid, section)
    self:MarkSent("DETAIL", guid .. ":" .. section, function() BI:RequestDetail(guid, section) end)
end

--- The account's other characters, online or not.
function BI:RequestAlts()
    self:Send("ALTS")
    self:MarkSent("ALTS", "0", function() BI:RequestAlts() end)
end

--- Act on one alt: "ADD" (log it in as a bot and party it), "INVITE", or "REMOVE".
-- Deliberately not retried by MarkSent. Every other request is a read and re-sending one costs
-- nothing, but these change the world -- a silent retry could log a character in twice or dismiss
-- one the player had just re-added.
function BI:RequestAltControl(action, guid)
    self:Send("ALTCTL", action, guid)
end

BI.SECTIONS = { "CORE", "GEAR", "SKILL", "QUEST" }

--- Sections fetched only when their tab is opened.
-- RECIPE is an order of magnitude larger than the rest -- a maxed crafter runs to tens of messages
-- where every other section is one or two -- and most sessions never look at it.
BI.LAZY_SECTIONS = { RECIPE = true }

--- Fetch one lazy section, unless it is already held for this bot.
function BI:RequestLazy(guid, section)
    local d = self.detail[guid]
    if d and d[section] then return false end
    self:RequestDetail(guid, section)
    return true
end

--- Request every eagerly-loaded section for one bot.
-- RECIPE is deliberately absent: it is an order of magnitude larger than the rest and is fetched
-- only when its tab is opened (A5).
function BI:RequestBot(guid)
    -- Hold exactly one bot's detail at a time. Two reasons, both found by testing rather than
    -- reasoning: nothing can then render a previous bot's data by mistake, and clicking through a
    -- few hundred bots over a session cannot quietly accumulate all of their detail. The cache was
    -- never read across bots anyway -- a re-select always refetches -- so keeping the rest bought
    -- nothing but a leak.
    self.detail = {}
    for _, section in ipairs(self.SECTIONS) do
        self:RequestDetail(guid, section)
    end
end

--- DETAIL section parsers.
--
-- These live in the transport layer rather than the UI so they can be exercised offline against the
-- exact bytes the server emits, without a running client. Every one of them takes the raw row list
-- dispatch() assembled and returns a plain table.

local CORE_SCALAR = {
    level = true, class = true, race = true, armor = true, gold = true, zone = true,
    str = true, agi = true, sta = true, int = true, spi = true,
}
local CORE_PAIR = { xp = true, hp = true, mana = true }

function BI.parseCore(rows)
    local core = { res = {} }
    for _, row in ipairs(rows) do
        local f = split(row, ":")
        local k = f[1]
        if k == "name" then
            core.name = f[2]
        elseif CORE_SCALAR[k] then
            core[k] = tonumber(f[2])
        elseif CORE_PAIR[k] then
            core[k] = { tonumber(f[2]) or 0, tonumber(f[3]) or 0 }
        elseif k == "res" then
            core.res = {
                fire   = tonumber(f[2]) or 0, nature = tonumber(f[3]) or 0,
                frost  = tonumber(f[4]) or 0, shadow = tonumber(f[5]) or 0,
                arcane = tonumber(f[6]) or 0,
            }
        end
    end
    return core
end

function BI.parseGear(rows)
    local out = {}
    for _, row in ipairs(rows) do
        local f = split(row, ":")
        local slot, item = tonumber(f[1]), tonumber(f[2])
        if slot and item then
            table.insert(out, { slot = slot, item = item,
                                suffix = tonumber(f[3]) or 0, enchant = tonumber(f[4]) or 0 })
        end
    end
    return out
end

function BI.parseSkill(rows)
    local out = {}
    for _, row in ipairs(rows) do
        local f = split(row, ":")
        local id = tonumber(f[1])
        if id then
            table.insert(out, { id = id, value = tonumber(f[2]) or 0, max = tonumber(f[3]) or 0 })
        end
    end
    return out
end

function BI.parseQuest(rows)
    -- The array part is the quest list; .zones rides alongside it as a hash entry, so `#quests`
    -- still counts quests and the grouping data does not need a second round trip.
    local out = { zones = {} }
    for _, row in ipairs(rows) do
        local f = split(row, ":")

        if f[1] == "#z" then
            -- Zone header: sent once per distinct zone in the log, because the client can no more
            -- name a quest's zone than it can name its own.
            out.zones[tonumber(f[2]) or 0] = f[3] or "Other"
        else
            local id = tonumber(f[1])
            if id then
                -- The title is everything from field 6 on, rejoined: quest names contain colons
                -- and splitting one into pieces would truncate every such quest at its punctuation.
                table.insert(out, {
                    id = id, status = tonumber(f[2]) or 0, level = tonumber(f[3]) or 0,
                    zone = tonumber(f[4]) or 0, objectives = f[5] or "-",
                    title = table.concat(f, ":", 6),
                })
            end
        end
    end
    return out
end

function BI.parseRecipe(rows)
    -- .truncated rides alongside the array part, like parseQuest's .zones.
    local out = { truncated = nil }
    for _, row in ipairs(rows) do
        local f = split(row, ":")
        if f[1] == "#more" then
            out.truncated = tonumber(f[2]) or 0
        else
            local skill, spell = tonumber(f[1]), tonumber(f[2])
            if skill and spell then
                table.insert(out, { skill = skill, spell = spell,
                                    min = tonumber(f[3]) or 0, grey = tonumber(f[4]) or 0 })
            end
        end
    end
    return out
end

BI.PARSERS = {
    CORE = BI.parseCore, GEAR = BI.parseGear, SKILL = BI.parseSkill, QUEST = BI.parseQuest,
    RECIPE = BI.parseRecipe,
}

--- Called once a multi-chunk response is fully assembled.
local function dispatch(verb, key, rows)
    clearInflight(verb, key)

    if verb == "ERR" then
        BI.lastError = rows[1] or "unknown error"
        if handlers.OnError then handlers.OnError(key, BI.lastError) end
        return
    end

    if verb == "ZONES" then
        BI.zones, BI.zoneNames = {}, {}
        for _, row in ipairs(rows) do
            local f = split(row, ":")
            if f[1] == "#gm" then
                -- What the server says this session may actually do. Asked for and answered rather
                -- than inferred: the inspector's own GM gate is a different check from the RBAC
                -- permissions that govern .appear and .summon, and assuming they move together
                -- would mean offering buttons that silently fail.
                BI.can = { appear = f[2] == "1", summon = f[3] == "1" }
            else
                local id = tonumber(f[1])
                if id then
                    BI.zones[id]     = tonumber(f[2]) or 0
                    BI.zoneNames[id] = f[3] or ("zone " .. id)
                end
            end
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

    elseif verb == "ALTS" then
        local list = {}
        for _, row in ipairs(rows) do
            local f = split(row, ":")
            if f[1] and f[1] ~= "" then
                table.insert(list, {
                    guid = tonumber(f[1]), name = f[2], level = tonumber(f[3]),
                    class = tonumber(f[4]), race = tonumber(f[5]),
                    -- offline | player | bot | party
                    state = f[6] or "offline",
                })
            end
        end
        BI.alts = list
        if handlers.OnAlts then handlers.OnAlts(list) end

    elseif verb == "ALTCTL" then
        local f = split(rows[1] or "", ":")
        if handlers.OnAltControl then
            handlers.OnAltControl(tonumber(f[1]), f[2] or "")
        end

    elseif verb == "DETAIL" then
        local f = split(key, ":")
        local guid, section = tonumber(f[1]), f[2]
        local parse = BI.PARSERS[section]

        BI.detail[guid] = BI.detail[guid] or { fetchedAt = {} }
        BI.detail[guid][section]   = parse and parse(rows) or rows
        -- Per-section timestamps, not one per bot. Sections arrive separately and can be refreshed
        -- separately, so a single stamp would label three fresh sections with the age of the fourth.
        BI.detail[guid].fetchedAt[section] = GetTime()

        if handlers.OnDetail then handlers.OnDetail(guid, section, BI.detail[guid][section]) end
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
        -- startedAt lets the sweep below reclaim an assembly whose last chunk never lands. Without
        -- it a buffer was only ever freed by completion or by a timeout on a request still marked
        -- in flight, so a truncated response left rows behind for the rest of the session.
        buf = { rows = {}, total = total, startedAt = GetTime() }
        pending[id] = buf
    end

    for i = 6, #fields do
        if fields[i] ~= "" then table.insert(buf.rows, fields[i]) end
    end

    -- A response is bounded by what the server will send: RECIPE caps at 400 rows and everything
    -- else is far smaller. Anything past this is a malformed or hostile stream, and growing a table
    -- to match it is how an addon turns a bad packet into a client that has to be restarted.
    if #buf.rows > MAX_ASSEMBLY_ROWS then
        pending[id] = nil
        clearInflight(verb, key)
        BI.lastError = "response too large, discarded"
        if handlers.OnError then handlers.OnError(key, BI.lastError) end
        return
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

-- Retries, not attempts. One means: send, and if that is not answered, send once more, then report.
local MAX_RETRIES = 1

--- Note a request as outstanding, remembering how to reissue it.
--
-- `tries` is carried across from any existing entry, because a retry goes back through the same
-- Request* call that lands here -- without that the counter would reset on every attempt and a
-- dead server would be retried forever.
function BI:MarkSent(verb, key, resend)
    local id = verb .. ":" .. (key or "")
    local prev = inflight[id]
    inflight[id] = { at = GetTime(), tries = (prev and prev.tries) or 0, resend = resend }
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

    for id, entry in pairs(inflight) do
        if now - entry.at > REQUEST_TIMEOUT then
            if entry.tries < MAX_RETRIES and entry.resend then
                -- One silent retry. A single dropped message is common enough that reporting a
                -- failure on the first miss trains the operator to ignore the message.
                entry.tries = entry.tries + 1
                entry.at    = now
                pending[id] = nil
                local ok = pcall(entry.resend)
                if not ok then
                    inflight[id] = nil
                    if handlers.OnTimeout then handlers.OnTimeout(id) end
                end
            else
                inflight[id] = nil
                pending[id]  = nil
                if handlers.OnTimeout then handlers.OnTimeout(id) end
            end
        end
    end

    -- Sweep assemblies nothing is waiting on any more. An unsolicited or truncated response is
    -- never in `inflight`, so the loop above cannot reclaim it; over a long session those are the
    -- buffers that accumulate.
    for id, buf in pairs(pending) do
        if not inflight[id] and now - (buf.startedAt or now) > REQUEST_TIMEOUT * 2 then
            pending[id] = nil
        end
    end
end)

--- Counts held by the assembly layer, for tests and for /bi debug.
function BI:Stats()
    local p, i = 0, 0
    for _ in pairs(pending)  do p = p + 1 end
    for _ in pairs(inflight) do i = i + 1 end
    return p, i
end

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
