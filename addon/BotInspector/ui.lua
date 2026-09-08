-- Bot Inspector -- A3 interface.
--
-- Still plain: styling, the paperdoll gear layout and real tabs are A4. A3's job is that all four
-- DETAIL sections arrive, parse and render truthfully -- including the cases that make an inspector
-- lie, namely stale data from a previously selected bot and items the client has never cached.

local BI = BotInspector

local ROW_HEIGHT, MAX_ROWS = 14, 22
local ROW_TOP = -76                 -- leaves room for the detail header and section buttons
local selectedZone, selectedBot, activeSection = nil, nil, "CORE"
local viewMode = "roster"           -- "roster" | "detail" | "find"

-- ---------------------------------------------------------------------------------------------
-- Static lookups. The client cannot resolve any of these from an id on its own.
-- ---------------------------------------------------------------------------------------------

local CLASS_TOKEN = {
    [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE",  [5] = "PRIEST",
    [6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}
local RACE_NAME = {
    [1] = "Human", [2] = "Orc", [3] = "Dwarf", [4] = "Night Elf", [5] = "Undead",
    [6] = "Tauren", [7] = "Gnome", [8] = "Troll", [10] = "Blood Elf", [11] = "Draenei",
}
local SKILL_NAME = {
    [129] = "First Aid",   [164] = "Blacksmithing", [165] = "Leatherworking", [171] = "Alchemy",
    [182] = "Herbalism",   [185] = "Cooking",       [186] = "Mining",         [197] = "Tailoring",
    [202] = "Engineering", [333] = "Enchanting",    [356] = "Fishing",        [393] = "Skinning",
    [755] = "Jewelcrafting", [773] = "Inscription",
}
local SLOT_NAME = {
    [0] = "Head", [1] = "Neck", [2] = "Shoulder", [3] = "Shirt", [4] = "Chest", [5] = "Waist",
    [6] = "Legs", [7] = "Feet", [8] = "Wrist", [9] = "Hands", [10] = "Ring 1", [11] = "Ring 2",
    [12] = "Trinket 1", [13] = "Trinket 2", [14] = "Back", [15] = "Main Hand", [16] = "Off Hand",
    [17] = "Ranged", [18] = "Tabard",
}
-- Only the values the server can actually report for a quest in a bot's log.
local QUEST_STATUS = { [0] = "none", [1] = "COMPLETE", [3] = "in progress", [5] = "FAILED", [6] = "rewarded" }

local function classColor(classId)
    local token = CLASS_TOKEN[classId or 0]
    local c = token and RAID_CLASS_COLORS and RAID_CLASS_COLORS[token]
    if c then return c.r, c.g, c.b end
    return 1, 1, 1
end

--- Quest difficulty relative to the *bot*, not to the operator looking at the panel.
-- GetQuestDifficultyColor would colour against the viewer's level, which is the wrong question
-- entirely: a level 80 operator inspecting a level 12 bot would see every quest greyed out.
local function questColor(questLevel, botLevel)
    local d = (questLevel or 0) - (botLevel or 0)
    if d >= 5 then return 1.0, 0.1, 0.1
    elseif d >= 3 then return 1.0, 0.5, 0.25
    elseif d >= -2 then return 1.0, 1.0, 0.0
    elseif d >= -5 then return 0.25, 0.75, 0.25
    else return 0.5, 0.5, 0.5 end
end

local function commify(n)
    n = tostring(math.floor(tonumber(n) or 0))
    local out = n:reverse():gsub("(%d%d%d)", "%1,"):reverse()
    return (out:gsub("^,", ""))
end

local function money(copper)
    copper = tonumber(copper) or 0
    return string.format("%dg %ds %dc", math.floor(copper / 10000),
                         math.floor(copper % 10000 / 100), copper % 100)
end

-- ---------------------------------------------------------------------------------------------
-- Frame
-- ---------------------------------------------------------------------------------------------

local frame = CreateFrame("Frame", "BotInspectorFrame", UIParent)
frame:SetWidth(560); frame:SetHeight(420)
frame:SetPoint("CENTER")
frame:SetBackdrop({
    bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
frame:SetMovable(true); frame:EnableMouse(true)
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", frame.StartMoving)
frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
frame:Hide()

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOP", 0, -16)
title:SetText("Bot Inspector")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -8, -8)

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
status:SetPoint("BOTTOMLEFT", 20, 18)
status:SetWidth(400); status:SetJustifyH("LEFT")
status:SetText("")

local refresh = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
refresh:SetWidth(80); refresh:SetHeight(20)
refresh:SetPoint("BOTTOMRIGHT", -18, 14)
refresh:SetText("Refresh")

-- Detail header: who you are looking at. Blank in roster mode.
local header = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
header:SetPoint("TOPLEFT", 210, -40)
header:SetWidth(330); header:SetJustifyH("LEFT")

local subheader = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
subheader:SetPoint("TOPLEFT", 210, -54)
subheader:SetWidth(330); subheader:SetJustifyH("LEFT")

-- ---------------------------------------------------------------------------------------------
-- Rows
-- ---------------------------------------------------------------------------------------------

local leftRows, rightRows = {}, {}

local function makeRow(parent, index, xOffset, width, top)
    local btn = CreateFrame("Button", nil, parent)
    btn:SetWidth(width); btn:SetHeight(ROW_HEIGHT)
    btn:SetPoint("TOPLEFT", xOffset, top - (index - 1) * ROW_HEIGHT)
    local text = btn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    text:SetPoint("LEFT", 4, 0)
    text:SetJustifyH("LEFT")
    btn.text = text
    btn:SetScript("OnEnter", function(self)
        if self.link then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink(self.link)
            GameTooltip:Show()
        end
    end)
    btn:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return btn
end

for i = 1, MAX_ROWS + 2 do
    leftRows[i] = makeRow(frame, i, 22, 180, -40)
end
for i = 1, MAX_ROWS do
    rightRows[i] = makeRow(frame, i, 210, 330, ROW_TOP)
end

local function clear(rows)
    for _, r in ipairs(rows) do
        r.text:SetText("")
        r.text:SetTextColor(1, 1, 1)
        r:SetScript("OnClick", nil)
        r.link = nil
    end
end

local function setRow(i, text, r, g, b, link, onClick)
    if i > MAX_ROWS then return end
    local row = rightRows[i]
    row.text:SetText(text)
    row.text:SetTextColor(r or 1, g or 1, b or 1)
    row.link = link
    row:SetScript("OnClick", onClick)
end

-- ---------------------------------------------------------------------------------------------
-- Section buttons
-- ---------------------------------------------------------------------------------------------

local sectionButtons = {}
local SECTION_LABEL = { CORE = "Stats", GEAR = "Gear", SKILL = "Profs", QUEST = "Quests" }

local drawDetail   -- forward declaration; the buttons below call it before it is defined

for i, section in ipairs(BI.SECTIONS) do
    local b = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    b:SetWidth(74); b:SetHeight(18)
    b:SetPoint("TOPLEFT", 208 + (i - 1) * 78, -58)
    b:SetText(SECTION_LABEL[section])
    b:SetScript("OnClick", function()
        activeSection = section
        drawDetail()
    end)
    b:Hide()
    sectionButtons[section] = b
end

local function showSectionButtons(show)
    for _, b in pairs(sectionButtons) do
        if show then b:Show() else b:Hide() end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Section renderers
-- ---------------------------------------------------------------------------------------------

local function renderCore(core)
    local i = 0
    local function line(fmt, ...)
        i = i + 1; setRow(i, string.format(fmt, ...))
    end

    local xp = core.xp or { 0, 0 }
    line("Level %d   %s", core.level or 0, money(core.gold or 0))
    line("XP  %s / %s  (%d%%)", commify(xp[1]), commify(xp[2]),
         xp[2] > 0 and math.floor(xp[1] / xp[2] * 100) or 0)
    i = i + 1
    line("Health   %s / %s", commify((core.hp or {})[1]), commify((core.hp or {})[2]))
    if (core.mana or { 0, 0 })[2] > 0 then
        line("Mana     %s / %s", commify(core.mana[1]), commify(core.mana[2]))
    end
    i = i + 1
    line("Strength   %4d      Agility   %4d", core.str or 0, core.agi or 0)
    line("Stamina    %4d      Intellect %4d", core.sta or 0, core["int"] or 0)
    line("Spirit     %4d      Armor     %4d", core.spi or 0, core.armor or 0)
    i = i + 1
    local r = core.res or {}
    line("Resist  F %d  N %d  Fr %d  S %d  A %d",
         r.fire or 0, r.nature or 0, r.frost or 0, r.shadow or 0, r.arcane or 0)
    i = i + 1
    line("Zone  %s", BI.zoneNames[core.zone or 0] or ("zone " .. tostring(core.zone)))
end

-- Items the client has never seen return nil from GetItemInfo. That is normal, not an error: the
-- call itself asks the server for the data. So render what we have, then redraw when it lands.
local pendingItems, itemRetries = false, 0

local function renderGear(gear)
    pendingItems = false

    if #gear == 0 then
        setRow(1, "no equipped items", 0.6, 0.6, 0.6)
        return
    end

    for i, g in ipairs(gear) do
        local link = string.format("item:%d:%d:0:0:0:0:%d:0", g.item, g.enchant, g.suffix)
        local name, _, quality = GetItemInfo(g.item)
        local label, r, gr, b

        if name then
            local c = ITEM_QUALITY_COLORS and ITEM_QUALITY_COLORS[quality or 1]
            r, gr, b = (c and c.r) or 1, (c and c.g) or 1, (c and c.b) or 1
            label = string.format("%-10s %s%s", SLOT_NAME[g.slot] or g.slot, name,
                                  g.enchant > 0 and "  +ench" or "")
        else
            r, gr, b = 0.6, 0.6, 0.6
            label = string.format("%-10s item %d  (loading)", SLOT_NAME[g.slot] or g.slot, g.item)
            pendingItems = true
        end

        setRow(i, label, r, gr, b, link)
    end

    if pendingItems then itemRetries = 0 end
end

local function renderSkill(skills)
    if #skills == 0 then
        setRow(1, "no professions or secondary skills", 0.6, 0.6, 0.6)
        return
    end

    table.sort(skills, function(a, b) return a.value > b.value end)

    for i, s in ipairs(skills) do
        local pct = s.max > 0 and s.value / s.max or 0
        local filled = math.floor(pct * 10 + 0.5)
        local bar = string.rep("#", filled) .. string.rep("-", 10 - filled)
        -- Amber once a skill is capped: that is the bot that needs a trainer, and it is the single
        -- most common reason a crafter stops making progress.
        local r, g, b = 1, 1, 1
        if s.value >= s.max and s.max > 0 then r, g, b = 1, 0.82, 0 end
        setRow(i, string.format("%-15s %3d / %3d  [%s]", SKILL_NAME[s.id] or ("skill " .. s.id),
                                s.value, s.max, bar), r, g, b)
    end
end

local function renderQuest(quests, botLevel)
    if #quests == 0 then
        setRow(1, "quest log empty", 0.6, 0.6, 0.6)
        return
    end

    -- Complete first, then in progress: "what can this bot turn in right now" is the question that
    -- gets asked, and burying it under twenty in-progress entries answers a different one.
    table.sort(quests, function(a, b)
        if a.status ~= b.status then return a.status == 1 end
        return a.level > b.level
    end)

    for i, q in ipairs(quests) do
        local r, g, b = questColor(q.level, botLevel)
        if q.status == 1 then r, g, b = 0.2, 1, 0.2 end

        -- Progress if there is any, otherwise the status word. A quest with no counted objective
        -- (talk to someone, reach a place) has nothing to show but whether it is done.
        local trailer
        if q.objectives ~= "-" and q.objectives ~= "" then
            trailer = "  " .. q.objectives
        else
            trailer = "  (" .. (QUEST_STATUS[q.status] or ("status " .. q.status)) .. ")"
        end

        setRow(i, string.format("[%d] %s%s", q.level, q.title ~= "" and q.title or ("quest " .. q.id),
                                trailer), r, g, b)
    end
end

-- ---------------------------------------------------------------------------------------------
-- Detail view
-- ---------------------------------------------------------------------------------------------

function drawDetail()
    if not selectedBot then return end
    clear(rightRows)
    showSectionButtons(true)

    local d = BI.detail[selectedBot]
    local core = d and d.CORE

    if core then
        local r, g, b = classColor(core.class)
        header:SetText(core.name or ("bot " .. selectedBot))
        header:SetTextColor(r, g, b)
        subheader:SetText(string.format("Level %d %s %s", core.level or 0,
                          RACE_NAME[core.race or 0] or "?",
                          (CLASS_TOKEN[core.class or 0] or "?"):lower()))
    else
        header:SetText("bot " .. selectedBot)
        header:SetTextColor(1, 1, 1)
        subheader:SetText("")
    end

    local data = d and d[activeSection]

    if not data then
        -- Never fall back to another bot's data, and never leave the previous bot's rows on screen.
        -- Silently showing stale values is the single commonest way a panel like this lies.
        setRow(1, "loading " .. SECTION_LABEL[activeSection] .. "...", 0.6, 0.6, 0.6)
        status:SetText("waiting for " .. activeSection)
        return
    end

    if activeSection == "CORE" then
        renderCore(data)
    elseif activeSection == "GEAR" then
        renderGear(data)
    elseif activeSection == "SKILL" then
        renderSkill(data)
    elseif activeSection == "QUEST" then
        renderQuest(data, core and core.level or 0)
    end

    local age = d.fetchedAt and d.fetchedAt[activeSection]
    status:SetText(string.format("%s -- %d rows, fetched %ds ago",
                   SECTION_LABEL[activeSection], #data, age and math.floor(GetTime() - age) or 0))
end

local function selectBot(guid, name)
    selectedBot = guid
    activeSection = "CORE"
    header:SetText(name or ("bot " .. guid)); header:SetTextColor(1, 1, 1)
    subheader:SetText("")
    viewMode = "detail"
    BI:RequestBot(guid)
    drawDetail()
end

-- ---------------------------------------------------------------------------------------------
-- Roster views
-- ---------------------------------------------------------------------------------------------

local function drawZones()
    clear(leftRows)
    local ordered = {}
    for zoneId, count in pairs(BI.zones) do table.insert(ordered, { id = zoneId, n = count }) end
    table.sort(ordered, function(a, b) return a.n > b.n end)

    for i = 1, math.min(#ordered, MAX_ROWS + 2) do
        local z = ordered[i]
        leftRows[i].text:SetText(string.format("%s (%d)", BI.zoneNames[z.id] or ("zone " .. z.id), z.n))
        leftRows[i]:SetScript("OnClick", function()
            selectedZone = z.id
            status:SetText("requesting roster...")
            BI:RequestList(z.id)
        end)
    end
    status:SetText(string.format("%d zones with bots", #ordered))
end

local function drawRoster(zoneId, list)
    viewMode = "roster"
    selectedBot = nil
    showSectionButtons(false)
    clear(rightRows)
    header:SetText(BI.zoneNames[zoneId] or ("zone " .. zoneId)); header:SetTextColor(1, 1, 1)
    subheader:SetText(string.format("%d bots", #list))

    table.sort(list, function(a, b) return a.level > b.level end)

    for i = 1, math.min(#list, MAX_ROWS) do
        local b = list[i]
        local r, g, bl = classColor(b.class)
        setRow(i, string.format("%-14s  lvl %2d  %dg", b.name, b.level, b.gold or 0), r, g, bl, nil,
               function() selectBot(b.guid, b.name) end)
    end
    status:SetText(string.format("%s: %d bots", BI.zoneNames[zoneId] or ("zone " .. zoneId), #list))
end

-- ---------------------------------------------------------------------------------------------
-- Handlers
-- ---------------------------------------------------------------------------------------------

BI:SetHandler("OnZones", function() drawZones() end)
BI:SetHandler("OnList",  function(zoneId, list) drawRoster(zoneId, list) end)

BI:SetHandler("OnDetail", function(guid, section)
    -- A late reply for a bot the operator has already clicked away from must not redraw the pane.
    if guid ~= selectedBot then return end
    if section == activeSection or section == "CORE" then drawDetail() end
end)

BI:SetHandler("OnFind", function(list, matched)
    viewMode = "find"
    selectedBot = nil
    showSectionButtons(false)
    clear(rightRows)
    header:SetText("Search results"); header:SetTextColor(1, 1, 1)
    subheader:SetText(string.format("%d of %d", #list, matched))

    for i = 1, math.min(#list, MAX_ROWS) do
        local b = list[i]
        setRow(i, string.format("%-14s  lvl %2d  %s", b.name, b.level,
               BI.zoneNames[b.zone] or ("zone " .. tostring(b.zone))), 1, 1, 1, nil,
               function() selectBot(b.guid, b.name) end)
    end
    status:SetText(string.format("showing %d of %d matches", #list, matched))
end)

BI:SetHandler("OnError", function(_, text) status:SetText("error: " .. text) end)
BI:SetHandler("OnTimeout", function(id)
    status:SetText("no reply to " .. id .. " -- server has no inspector, or refused (needs GM 2)")
end)

refresh:SetScript("OnClick", function()
    if viewMode == "detail" and selectedBot then
        status:SetText("refreshing bot...")
        BI:RequestBot(selectedBot)
        drawDetail()
    elseif viewMode == "roster" and selectedZone then
        status:SetText("refreshing roster...")
        BI:RequestList(selectedZone)
    else
        status:SetText("requesting zones...")
        BI:RequestZones()
    end
end)

-- Redraw gear once the server answers the item queries GetItemInfo triggered. Polling rather than
-- listening for GET_ITEM_INFO_RECEIVED, because that event's presence varies across 3.3.5 builds
-- and a half-second retry is both simpler and independent of it. Bounded so an item that never
-- resolves costs ten seconds, not an OnUpdate forever.
local itemWatch = CreateFrame("Frame")
itemWatch:SetScript("OnUpdate", function(self, elapsed)
    if not pendingItems then return end
    self.acc = (self.acc or 0) + elapsed
    if self.acc < 0.5 then return end
    self.acc = 0

    itemRetries = itemRetries + 1
    if itemRetries > 20 then pendingItems = false; return end
    if viewMode == "detail" and activeSection == "GEAR" then drawDetail() end
end)

function BI:Toggle()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
        status:SetText("requesting zones...")
        self:RequestZones()
    end
end
