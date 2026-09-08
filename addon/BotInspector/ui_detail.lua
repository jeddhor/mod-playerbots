-- Bot Inspector -- right pane: the character sheet.
--
-- Four sections with genuinely different shapes, so four layouts rather than one table: stats in
-- columns, gear in the paperdoll arrangement, professions as bars against their cap, quests grouped
-- by zone. Each keeps its own frame and is shown or hidden; nothing is rebuilt on a tab switch.

local BI = BotInspector
local W  = BI.W
local UI = BI.UI

local PANE_W = UI.RIGHT_W
local PANE_H = UI.right:GetHeight()
local CONTENT_TOP = -72

local SECTION_LABEL = { CORE = "Stats", GEAR = "Gear", SKILL = "Profs", QUEST = "Quests" }
local QUEST_STATUS  = { [0] = "none", [1] = "COMPLETE", [3] = "in progress", [5] = "FAILED", [6] = "rewarded" }

-- Blizzard's own paperdoll order. Weapons sit across the bottom; the 3D model the operator did not
-- want is what normally occupies the space between the columns, so the gear summary goes there.
local LEFT_SLOTS   = { 0, 1, 2, 14, 4, 3, 18, 8 }
local RIGHT_SLOTS  = { 9, 5, 6, 7, 10, 11, 12, 13 }
local BOTTOM_SLOTS = { 15, 16, 17 }
local SLOT_NAME = {
    [0] = "Head", [1] = "Neck", [2] = "Shoulder", [3] = "Shirt", [4] = "Chest", [5] = "Waist",
    [6] = "Legs", [7] = "Feet", [8] = "Wrist", [9] = "Hands", [10] = "Ring 1", [11] = "Ring 2",
    [12] = "Trinket 1", [13] = "Trinket 2", [14] = "Back", [15] = "Main Hand", [16] = "Off Hand",
    [17] = "Ranged", [18] = "Tabard",
}

local drawSection   -- forward declared; tabs and handlers below call it

-- ---------------------------------------------------------------------------------------------
-- Header and tabs
-- ---------------------------------------------------------------------------------------------

local name = UI.right:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
name:SetPoint("TOPLEFT", 12, -10)
name:SetJustifyH("LEFT")

local subtitle = UI.right:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
subtitle:SetPoint("TOPLEFT", 12, -32)
subtitle:SetJustifyH("LEFT")

local fetched = UI.right:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
fetched:SetPoint("TOPRIGHT", -12, -32)
fetched:SetJustifyH("RIGHT")

local tabs = {}
for i, section in ipairs(BI.SECTIONS) do
    local b = CreateFrame("Button", nil, UI.right, "UIPanelButtonTemplate")
    b:SetWidth(84); b:SetHeight(20)
    b:SetPoint("TOPLEFT", 10 + (i - 1) * 88, -50)
    b:SetText(SECTION_LABEL[section])
    b:SetScript("OnClick", function()
        UI.activeSection = section
        drawSection()
    end)
    tabs[section] = b
end

local function paintTabs()
    for section, b in pairs(tabs) do
        -- The pushed state is how a plain button shows selection without needing a tab template,
        -- which may not exist on every 3.3.5 build and would take the addon down at load if absent.
        if section == UI.activeSection then
            b:SetButtonState("PUSHED", true)
        else
            b:SetButtonState("NORMAL", false)
        end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Section frames
-- ---------------------------------------------------------------------------------------------

local function contentFrame()
    local f = CreateFrame("Frame", nil, UI.right)
    f:SetPoint("TOPLEFT", 8, CONTENT_TOP)
    f:SetWidth(PANE_W - 16); f:SetHeight(PANE_H + CONTENT_TOP - 8)
    f:Hide()
    return f
end

local panes = { CORE = contentFrame(), GEAR = contentFrame(), SKILL = contentFrame(), QUEST = contentFrame() }

local hint = UI.right:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
hint:SetPoint("CENTER", UI.right, "CENTER", 0, 0)
hint:SetText("select a bot from a zone on the left")

-- ---- Stats -----------------------------------------------------------------------------------

local statRows = {}
local STAT_LEFT  = { { "Strength", "str" }, { "Agility", "agi" }, { "Stamina", "sta" },
                     { "Intellect", "int" }, { "Spirit", "spi" } }
local STAT_RIGHT = { { "Health", "hp" }, { "Mana", "mana" }, { "Armor", "armor" }, { "Gold", "gold" } }

local function makeStatRow(parent, i, x)
    local label = parent:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    label:SetPoint("TOPLEFT", x, -8 - (i - 1) * 18)
    local value = parent:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    value:SetPoint("TOPLEFT", x + 92, -8 - (i - 1) * 18)
    return { label = label, value = value }
end

for i = 1, #STAT_LEFT  do statRows["L" .. i] = makeStatRow(panes.CORE, i, 12) end
for i = 1, #STAT_RIGHT do statRows["R" .. i] = makeStatRow(panes.CORE, i, 232) end

local resistLine = panes.CORE:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
resistLine:SetPoint("TOPLEFT", 12, -8 - 5 * 18 - 12)
resistLine:SetJustifyH("LEFT")

local xpBar = W.Bar(panes.CORE, PANE_W - 40, 14)
xpBar:SetPoint("TOPLEFT", 12, -8 - 5 * 18 - 40)
xpBar.bar:SetStatusBarColor(0.35, 0.15, 0.55)

local zoneLine = panes.CORE:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
zoneLine:SetPoint("TOPLEFT", 12, -8 - 5 * 18 - 78)

local function renderCore(core)
    for i, def in ipairs(STAT_LEFT) do
        local row = statRows["L" .. i]
        row.label:SetText(def[1])
        row.value:SetText(W.Commify(core[def[2]] or 0))
    end

    local vals = {
        hp    = core.hp   and string.format("%s / %s", W.Commify(core.hp[1]), W.Commify(core.hp[2])) or "-",
        mana  = (core.mana and core.mana[2] > 0)
                and string.format("%s / %s", W.Commify(core.mana[1]), W.Commify(core.mana[2])) or "-",
        armor = W.Commify(core.armor or 0),
        gold  = W.Money(core.gold or 0),
    }
    for i, def in ipairs(STAT_RIGHT) do
        local row = statRows["R" .. i]
        row.label:SetText(def[1])
        row.value:SetText(vals[def[2]])
    end

    local r = core.res or {}
    resistLine:SetText(string.format("Resistances   fire %d   nature %d   frost %d   shadow %d   arcane %d",
                       r.fire or 0, r.nature or 0, r.frost or 0, r.shadow or 0, r.arcane or 0))

    local xp = core.xp or { 0, 0 }
    local pct = (xp[2] or 0) > 0 and xp[1] / xp[2] or 0
    xpBar.label:SetText("Experience")
    xpBar.value:SetText(string.format("%s / %s  (%d%%)", W.Commify(xp[1]), W.Commify(xp[2]), pct * 100))
    xpBar.bar:SetValue(pct)

    zoneLine:SetText("Zone: " .. (BI.zoneNames[core.zone or 0] or ("zone " .. tostring(core.zone))))
end

-- ---- Gear ------------------------------------------------------------------------------------

local slots = {}
local function placeSlots(list, x, yStart, step)
    for i, slotId in ipairs(list) do
        local s = W.ItemSlot(panes.GEAR, 36)
        s:SetPoint("TOPLEFT", x, yStart - (i - 1) * step)
        slots[slotId] = s
    end
end
placeSlots(LEFT_SLOTS, 14, -6, 40)
placeSlots(RIGHT_SLOTS, PANE_W - 66, -6, 40)
for i, slotId in ipairs(BOTTOM_SLOTS) do
    local s = W.ItemSlot(panes.GEAR, 36)
    s:SetPoint("TOPLEFT", (PANE_W - 16) / 2 - 62 + (i - 1) * 42, -6 - 8 * 40 - 4)
    slots[slotId] = s
end

local gearSummary = panes.GEAR:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
gearSummary:SetPoint("TOP", panes.GEAR, "TOP", 0, -110)
gearSummary:SetJustifyH("CENTER")

local gearPending, gearRetries = false, 0

local function renderGear(gear)
    gearPending = false

    local bySlot = {}
    for _, g in ipairs(gear) do bySlot[g.slot] = g end

    -- equipped counts items the bot is wearing; counted counts the ones whose item level the
    -- client could resolve. They differ whenever an item is not yet cached, and conflating them
    -- made an uncached item report itself as an empty slot.
    local total, counted, equipped, enchanted, enchantable = 0, 0, 0, 0, 0

    for slotId, slot in pairs(slots) do
        local g = bySlot[slotId]
        local ok = W.SetItemSlot(slot, g and g.item, g and g.enchant, g and g.suffix, SLOT_NAME[slotId])
        if not ok then gearPending = true end

        if g then
            equipped = equipped + 1
            local _, _, _, ilvl = GetItemInfo(g.item)
            if ilvl then total = total + ilvl; counted = counted + 1 end

            -- Shirt and tabard cannot be enchanted, so counting them would understate every bot.
            if slotId ~= 3 and slotId ~= 18 then
                enchantable = enchantable + 1
                if (g.enchant or 0) > 0 then enchanted = enchanted + 1 end
            end
            slot.label:SetText((g.enchant or 0) > 0 and "|cff40ff40*|r" or "")
        else
            slot.label:SetText("")
        end
    end

    gearSummary:SetText(string.format("%d equipped\navg ilvl %s\n%d/%d enchanted",
                        equipped,
                        counted > 0 and string.format("%.0f", total / counted) or "?",
                        enchanted, enchantable))

    if gearPending then gearRetries = 0 end
end

-- ---- Professions -----------------------------------------------------------------------------

local SKILL_NAME = {
    [129] = "First Aid",   [164] = "Blacksmithing", [165] = "Leatherworking", [171] = "Alchemy",
    [182] = "Herbalism",   [185] = "Cooking",       [186] = "Mining",         [197] = "Tailoring",
    [202] = "Engineering", [333] = "Enchanting",    [356] = "Fishing",        [393] = "Skinning",
    [755] = "Jewelcrafting", [773] = "Inscription",
}

local skillBars = {}
for i = 1, 14 do
    local bar = W.Bar(panes.SKILL, PANE_W - 40, 12)
    bar:SetPoint("TOPLEFT", 12, -8 - (i - 1) * 30)
    bar:Hide()
    skillBars[i] = bar
end

local skillEmpty = panes.SKILL:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
skillEmpty:SetPoint("TOPLEFT", 12, -10)

local function renderSkill(skills)
    for _, b in ipairs(skillBars) do b:Hide() end

    if #skills == 0 then
        skillEmpty:SetText("no professions or secondary skills")
        return
    end
    skillEmpty:SetText("")

    local sorted = {}
    for _, s in ipairs(skills) do table.insert(sorted, s) end
    table.sort(sorted, function(a, b) return a.value > b.value end)

    for i, s in ipairs(sorted) do
        local bar = skillBars[i]
        if not bar then break end
        bar:Show()
        bar.label:SetText(SKILL_NAME[s.id] or ("skill " .. s.id))
        bar.value:SetText(string.format("%d / %d", s.value, s.max))
        bar.bar:SetValue(s.max > 0 and s.value / s.max or 0)

        -- Amber at the cap: that bot needs a trainer, and it is the commonest reason a crafter
        -- quietly stops making progress.
        if s.max > 0 and s.value >= s.max then
            bar.bar:SetStatusBarColor(1, 0.82, 0)
        else
            bar.bar:SetStatusBarColor(0.2, 0.55, 0.85)
        end
    end
end

-- ---- Quests ----------------------------------------------------------------------------------

local QUEST_ROW_H = 14
local questRows, questItems = {}, {}
local QUEST_VISIBLE = math.floor((PANE_H + CONTENT_TOP - 16) / QUEST_ROW_H)

for i = 1, QUEST_VISIBLE do
    local r = W.Row(panes.QUEST, PANE_W - 24, QUEST_ROW_H)
    r:SetPoint("TOPLEFT", 8, -6 - (i - 1) * QUEST_ROW_H)
    questRows[i] = r
end

local function questColor(questLevel, botLevel)
    local d = (questLevel or 0) - (botLevel or 0)
    if d >= 5 then return 1.0, 0.1, 0.1
    elseif d >= 3 then return 1.0, 0.5, 0.25
    elseif d >= -2 then return 1.0, 1.0, 0.0
    elseif d >= -5 then return 0.25, 0.75, 0.25
    else return 0.5, 0.5, 0.5 end
end

local function drawQuestRows()
    local offset = panes.QUEST.offset or 0
    if offset > math.max(0, #questItems - QUEST_VISIBLE) then
        offset = math.max(0, #questItems - QUEST_VISIBLE)
        panes.QUEST.offset = offset
    end

    for i = 1, QUEST_VISIBLE do
        local row, item = questRows[i], questItems[i + offset]
        row.right:SetText("")
        if not item then
            row.text:SetText("")
            row:Hide()
        else
            row:Show()
            if item.kind == "zone" then
                row.text:SetText(item.text)
                row.text:SetTextColor(1, 0.82, 0)
            else
                row.text:SetText(item.text)
                row.text:SetTextColor(item.r, item.g, item.b)
                row.right:SetText(item.right or "")
            end
        end
    end
end

local function renderQuest(quests, botLevel)
    questItems = {}

    if #quests == 0 then
        questItems = { { kind = "note", text = "quest log empty", r = 0.6, g = 0.6, b = 0.6 } }
        drawQuestRows()
        return
    end

    -- Group by zone, but order the groups by how much of the bot's log sits in each: the zone it is
    -- actually working is the one worth seeing first.
    local byZone = {}
    for _, q in ipairs(quests) do
        byZone[q.zone] = byZone[q.zone] or {}
        table.insert(byZone[q.zone], q)
    end

    local order = {}
    for zoneId, list in pairs(byZone) do table.insert(order, { id = zoneId, list = list }) end
    table.sort(order, function(a, b)
        if #a.list == #b.list then return (quests.zones[a.id] or "") < (quests.zones[b.id] or "") end
        return #a.list > #b.list
    end)

    for _, group in ipairs(order) do
        table.insert(questItems, {
            kind = "zone",
            text = string.format("%s (%d)", quests.zones[group.id] or "Other", #group.list),
        })

        table.sort(group.list, function(a, b)
            if a.status ~= b.status then return a.status == 1 end
            return a.level > b.level
        end)

        for _, q in ipairs(group.list) do
            local r, g, b = questColor(q.level, botLevel)
            if q.status == 1 then r, g, b = 0.2, 1, 0.2 end

            local right = (q.objectives ~= "-" and q.objectives ~= "")
                          and q.objectives or (QUEST_STATUS[q.status] or "")
            table.insert(questItems, {
                kind = "quest", r = r, g = g, b = b, right = right,
                text = string.format("   [%d] %s", q.level, q.title ~= "" and q.title or ("quest " .. q.id)),
            })
        end
    end

    drawQuestRows()
end

W.MakeScrollable(panes.QUEST, function() return #questItems end,
                 function() return QUEST_VISIBLE end, drawQuestRows)

-- ---------------------------------------------------------------------------------------------
-- Orchestration
-- ---------------------------------------------------------------------------------------------

function drawSection()
    paintTabs()

    for _, p in pairs(panes) do p:Hide() end

    if not UI.selectedBot then
        hint:Show()
        name:SetText(""); subtitle:SetText(""); fetched:SetText("")
        return
    end
    hint:Hide()

    local d    = BI.detail[UI.selectedBot]
    local core = d and d.CORE

    if core then
        local r, g, b = W.ClassColor(core.class)
        name:SetText(core.name or ("bot " .. UI.selectedBot))
        name:SetTextColor(r, g, b)
        subtitle:SetText(string.format("Level %d %s %s", core.level or 0,
                         W.RACE_NAME[core.race or 0] or "?", W.CLASS_NAME[core.class or 0] or "?"))
    elseif UI.selectedName then
        name:SetText(UI.selectedName); name:SetTextColor(1, 1, 1)
        subtitle:SetText("")
    end

    local pane = panes[UI.activeSection]
    local data = d and d[UI.activeSection]

    if not data then
        pane:Hide()
        fetched:SetText("loading " .. SECTION_LABEL[UI.activeSection] .. "...")
        return
    end

    pane:Show()
    if     UI.activeSection == "CORE"  then renderCore(data)
    elseif UI.activeSection == "GEAR"  then renderGear(data)
    elseif UI.activeSection == "SKILL" then renderSkill(data)
    elseif UI.activeSection == "QUEST" then renderQuest(data, core and core.level or 0) end

    local at = d.fetchedAt and d.fetchedAt[UI.activeSection]
    fetched:SetText(at and string.format("fetched %ds ago", math.floor(GetTime() - at)) or "")
end

UI.DrawDetail = drawSection

function UI:SelectBot(guid, bot)
    self.selectedBot   = guid
    self.selectedName  = bot and bot.name
    self.activeSection = "CORE"
    panes.QUEST.offset = 0
    BI:RequestBot(guid)
    drawSection()
    self:SetStatus("requesting detail for %s...", (bot and bot.name) or ("bot " .. guid))
end

BI:SetHandler("OnDetail", function(guid, section)
    -- A reply for a bot the operator has already clicked away from must not redraw this pane.
    if guid ~= UI.selectedBot then return end
    if section == UI.activeSection or section == "CORE" then drawSection() end
end)

BI:SetHandler("OnError", function(_, text) UI:SetStatus("error: %s", text) end)
BI:SetHandler("OnTimeout", function(id)
    UI:SetStatus("no reply to %s -- server has no inspector, or refused (needs GM 2)", id)
end)

UI.refresh:SetScript("OnClick", function()
    if UI.selectedBot then
        UI:SetStatus("refreshing bot...")
        BI:RequestBot(UI.selectedBot)
        drawSection()
    else
        UI:SetStatus("requesting zones...")
        BI:RequestZones()
    end
end)

-- Items the client has never cached resolve a moment after GetItemInfo asks for them. Polling
-- rather than listening for GET_ITEM_INFO_RECEIVED, whose presence varies across 3.3.5 builds, and
-- bounded so an item that never resolves costs ten seconds instead of an OnUpdate forever.
local itemWatch = CreateFrame("Frame")
itemWatch:SetScript("OnUpdate", function(self, elapsed)
    if not gearPending then return end
    self.acc = (self.acc or 0) + elapsed
    if self.acc < 0.5 then return end
    self.acc = 0

    gearRetries = gearRetries + 1
    if gearRetries > 20 then gearPending = false; return end
    if UI.activeSection == "GEAR" then drawSection() end
end)

paintTabs()
