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
local CONTENT_TOP = -82

local SECTION_LABEL = { CORE = "Stats", GEAR = "Gear", SKILL = "Profs", QUEST = "Quests",
                        RECIPE = "Recipes" }

-- Tab order, which is not BI.SECTIONS: that list is what a bot selection fetches eagerly, and
-- RECIPE deliberately is not in it.
local TABS = { "CORE", "GEAR", "SKILL", "QUEST", "RECIPE" }
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
for i, section in ipairs(TABS) do
    local b = CreateFrame("Button", nil, UI.right, "UIPanelButtonTemplate")
    -- 24 rather than 20: at 20 the label sits hard against the button's own bevel.
    b:SetWidth(84); b:SetHeight(24)
    b:SetPoint("TOPLEFT", 10 + (i - 1) * 88, -52)
    b:SetText(SECTION_LABEL[section])
    b:SetScript("OnClick", function()
        UI.activeSection = section
        -- Opening the tab is what fetches a lazy section. Nothing else asks for it, and a second
        -- visit reuses what arrived the first time.
        if BI.LAZY_SECTIONS[section] and UI.selectedBot then
            if BI:RequestLazy(UI.selectedBot, section) then
                UI:SetStatus("fetching recipes -- this is the large one, give it a moment")
            end
        end
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

local panes = { CORE = contentFrame(), GEAR = contentFrame(), SKILL = contentFrame(),
                QUEST = contentFrame(), RECIPE = contentFrame() }

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

-- Resistances get real textures rather than inline glyphs, so they can carry the character
-- sheet's own border art and be sized independently of the font.
local RES_ORDER = { "fire", "nature", "frost", "shadow", "arcane" }
local RES_ICON_SIZE = 20
local resistLabel = panes.CORE:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
resistLabel:SetPoint("TOPLEFT", 12, -8 - 5 * 18 - 14)
resistLabel:SetText("Resistances")

local resistCells = {}
for i, school in ipairs(RES_ORDER) do
    local holder = CreateFrame("Frame", nil, panes.CORE)
    holder:SetWidth(RES_ICON_SIZE + 26); holder:SetHeight(RES_ICON_SIZE)
    holder:SetPoint("TOPLEFT", 96 + (i - 1) * (RES_ICON_SIZE + 30), -8 - 5 * 18 - 16)

    local tex = holder:CreateTexture(nil, "ARTWORK")
    tex:SetWidth(RES_ICON_SIZE); tex:SetHeight(RES_ICON_SIZE)
    tex:SetPoint("LEFT", 0, 0)
    if W.RES_BORDERED then
        tex:SetTexture(W.RES_SHEET)
        tex:SetTexCoord(W.ResCoords(school))
    else
        tex:SetTexture(W.RES_ICON[school])
    end

    local value = holder:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    value:SetPoint("LEFT", tex, "RIGHT", 4, 0)
    value:SetJustifyH("LEFT")

    holder.value = value
    holder.school = school
    resistCells[i] = holder
end

local xpBar = W.Bar(panes.CORE, PANE_W - 40, 14)
xpBar:SetPoint("TOPLEFT", 12, -8 - 5 * 18 - 48)
xpBar.bar:SetStatusBarColor(0.35, 0.15, 0.55)

local zoneLine = panes.CORE:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
zoneLine:SetPoint("TOPLEFT", 12, -8 - 5 * 18 - 86)

-- ---- GM controls ------------------------------------------------------------------------------
--
-- These drive the real .appear and .summon rather than a reimplementation. Those commands already
-- handle instance binds, battlegrounds and transports correctly, and a teleport written fresh here
-- would have to get all of that right a second time. A chat message beginning with "." is routed
-- to the command parser server-side before it can ever be spoken aloud.

local gmLabel = panes.CORE:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
gmLabel:SetPoint("BOTTOMLEFT", 12, 40)
gmLabel:SetText("Game Master")

local gmNote = panes.CORE:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
gmNote:SetPoint("BOTTOMLEFT", 12, 8)
gmNote:SetWidth(PANE_W - 40); gmNote:SetJustifyH("LEFT")

local function gmButton(text, x, command, permission)
    local b = CreateFrame("Button", nil, panes.CORE, "UIPanelButtonTemplate")
    b:SetWidth(140); b:SetHeight(24)
    b:SetPoint("BOTTOMLEFT", x, 14)
    b:SetText(text)
    b:SetScript("OnClick", function()
        local core = UI.selectedBot and BI.detail[UI.selectedBot] and BI.detail[UI.selectedBot].CORE
        if not core or not core.name then
            UI:SetStatus("no bot selected")
            return
        end
        -- Addon channel, not chat: the client slurs a drunk character's chat, so ".summon" left as
        -- ".shummon". The server runs the command for us as though it had been typed.
        BI:Send("CMD", string.upper(command), core.name)
        UI:SetStatus("sent .%s %s", command, core.name)
    end)
    b.permission = permission
    return b
end

local gmButtons = {
    gmButton("Go to bot",      12,  "appear", "appear"),
    gmButton("Bring bot to me", 160, "summon", "summon"),
}

local function paintGmButtons()
    local any = false
    for _, b in ipairs(gmButtons) do
        local allowed = BI.can and BI.can[b.permission]
        -- Disabled rather than hidden: a control that vanishes leaves the operator wondering
        -- whether the feature exists at all, where a greyed one says "not for this account".
        if allowed and UI.selectedBot then b:Enable() else b:Disable() end
        any = any or allowed
    end

    if not BI.can or not (BI.can.appear or BI.can.summon) then
        gmNote:SetText("account lacks the .appear / .summon permissions")
    elseif not UI.selectedBot then
        gmNote:SetText("select a bot to enable")
    else
        gmNote:SetText("")
    end
end

UI.PaintGmButtons = paintGmButtons

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
    for _, cell in ipairs(resistCells) do
        cell.value:SetText(tostring(r[cell.school] or 0))
    end

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

            -- The same "?" Blizzard puts over a turn-in NPC, so a finished quest is spotted by
            -- shape rather than by reading the whole line.
            local marker = q.status == 1 and W.Icon(W.QUEST_COMPLETE_ICON, 12) or "  "

            table.insert(questItems, {
                kind = "quest", r = r, g = g, b = b, right = right,
                text = string.format("  %s [%d] %s", marker, q.level,
                                     q.title ~= "" and q.title or ("quest " .. q.id)),
            })
        end
    end

    drawQuestRows()
end

W.MakeScrollable(panes.QUEST, function() return #questItems end,
                 function() return QUEST_VISIBLE end, drawQuestRows)

-- ---- Recipes ---------------------------------------------------------------------------------

local RECIPE_ROW_H = 14
local recipeRows, recipeItems = {}, {}
local RECIPE_VISIBLE = math.floor((PANE_H + CONTENT_TOP - 16) / RECIPE_ROW_H)

for i = 1, RECIPE_VISIBLE do
    local r = W.Row(panes.RECIPE, PANE_W - 24, RECIPE_ROW_H)
    r:SetPoint("TOPLEFT", 8, -6 - (i - 1) * RECIPE_ROW_H)
    recipeRows[i] = r
end

local function drawRecipeRows()
    local offset = panes.RECIPE.offset or 0
    if offset > math.max(0, #recipeItems - RECIPE_VISIBLE) then
        offset = math.max(0, #recipeItems - RECIPE_VISIBLE)
        panes.RECIPE.offset = offset
    end

    for i = 1, RECIPE_VISIBLE do
        local row, item = recipeRows[i], recipeItems[i + offset]
        row.right:SetText("")
        row.link = nil
        if not item then
            row.text:SetText("")
            row:Hide()
        else
            row:Show()
            row.text:SetText(item.text)
            row.text:SetTextColor(item.r, item.g, item.b)
            row.right:SetText(item.right or "")
            row.link = item.link
        end
    end
end

--- Trade-skill colouring, from the client's own convention.
-- A recipe is grey once the bot's skill has passed the rank at which it stops giving skill-ups,
-- which is the single fact that answers "why has this crafter stopped levelling".
local function recipeColor(botSkill, grey)
    if grey > 0 and botSkill >= grey then return 0.5, 0.5, 0.5, "trivial" end
    if grey > 0 and botSkill >= grey - 15 then return 0.25, 0.75, 0.25, "green" end
    if grey > 0 and botSkill >= grey - 30 then return 1.0, 1.0, 0.0, "yellow" end
    return 1.0, 0.5, 0.25, "orange"
end

local function renderRecipe(recipes, skills)
    recipeItems = {}

    -- The bot's current rank per skill, so each recipe can be coloured against it.
    local rank = {}
    for _, sk in ipairs(skills or {}) do rank[sk.id] = sk.value end

    if #recipes == 0 then
        recipeItems = { { text = "no crafting recipes known", r = 0.6, g = 0.6, b = 0.6 } }
        drawRecipeRows()
        return
    end

    local bySkill = {}
    for _, r in ipairs(recipes) do
        bySkill[r.skill] = bySkill[r.skill] or {}
        table.insert(bySkill[r.skill], r)
    end

    local order = {}
    for skillId, list in pairs(bySkill) do table.insert(order, { id = skillId, list = list }) end
    table.sort(order, function(a, b) return #a.list > #b.list end)

    for _, group in ipairs(order) do
        local known = rank[group.id]
        table.insert(recipeItems, {
            text = string.format("%s (%d)", SKILL_NAME[group.id] or ("skill " .. group.id), #group.list),
            r = 1, g = 0.82, b = 0,
            right = known and tostring(known) or "",
        })

        table.sort(group.list, function(a, b)
            if a.grey ~= b.grey then return a.grey > b.grey end
            return a.spell < b.spell
        end)

        for _, rec in ipairs(group.list) do
            -- GetSpellInfo resolves locally: every spell is in the client's own Spell.dbc, which is
            -- why the server sends bare ids here and nothing larger.
            local spellName = GetSpellInfo(rec.spell)
            local r, g, b, tier = recipeColor(known or 0, rec.grey)
            table.insert(recipeItems, {
                text = "   " .. (spellName or ("spell " .. rec.spell)),
                r = r, g = g, b = b,
                right = rec.grey > 0 and tostring(rec.grey) or "",
                link = "spell:" .. rec.spell,
            })
        end
    end

    if recipes.truncated then
        table.insert(recipeItems, {
            text = string.format("   ...list capped at %d", recipes.truncated),
            r = 0.7, g = 0.7, b = 0.7,
        })
    end

    drawRecipeRows()
end

W.MakeScrollable(panes.RECIPE, function() return #recipeItems end,
                 function() return RECIPE_VISIBLE end, drawRecipeRows)

-- ---------------------------------------------------------------------------------------------
-- Orchestration
-- ---------------------------------------------------------------------------------------------

function drawSection()
    paintTabs()

    for _, p in pairs(panes) do p:Hide() end

    if not UI.selectedBot then
        hint:Show()
        name:SetText(""); subtitle:SetText(""); fetched:SetText("")
        paintGmButtons()
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
    if     UI.activeSection == "CORE"   then renderCore(data); paintGmButtons()
    elseif UI.activeSection == "GEAR"   then renderGear(data)
    elseif UI.activeSection == "SKILL"  then renderSkill(data)
    elseif UI.activeSection == "QUEST"  then renderQuest(data, core and core.level or 0)
    elseif UI.activeSection == "RECIPE" then renderRecipe(data, d.SKILL) end

    local at = d.fetchedAt and d.fetchedAt[UI.activeSection]
    fetched:SetText(at and string.format("fetched %ds ago", math.floor(GetTime() - at)) or "")
end

UI.DrawDetail = drawSection

function UI:SelectBot(guid, bot)
    self.selectedBot   = guid
    self.selectedName  = bot and bot.name
    self.activeSection = "CORE"
    panes.QUEST.offset  = 0
    panes.RECIPE.offset = 0
    BI:RequestBot(guid)
    drawSection()
    self:SetStatus("requesting detail for %s...", (bot and bot.name) or ("bot " .. guid))
end

--- Ask the client for every gear item the moment the list arrives.
--
-- GetItemInfo is not only a lookup: calling it on an uncached item is what asks the server for that
-- item. Waiting until the Gear tab was opened meant the queries started at the instant their
-- answers were needed, so the first render was always question marks. GEAR is fetched eagerly on
-- selection, so priming here gives the round trip the seconds it takes to click a tab.
--
-- Nothing is done with the return value; the call itself is the point.
local function primeItemCache(gear)
    for _, g in ipairs(gear or {}) do
        if g.item and g.item ~= 0 then GetItemInfo(g.item) end
    end
end

BI:SetHandler("OnDetail", function(guid, section, data)
    -- A reply for a bot the operator has already clicked away from must not redraw this pane.
    if guid ~= UI.selectedBot then return end
    if section == "GEAR" then primeItemCache(data) end
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
    if self.acc < 0.1 then return end
    self.acc = 0

    -- 0.1s rather than 0.5s. GET_ITEM_INFO_RECEIVED does not exist on 3.3.5 -- verified by looking
    -- for the string in Wow.exe, it is not there -- so polling is the only route, and half a second
    -- was long enough to be visible as a question mark that "fixed itself".
    gearRetries = gearRetries + 1
    if gearRetries > 100 then gearPending = false; return end
    if UI.activeSection == "GEAR" then drawSection() end
end)

paintTabs()

paintGmButtons()
