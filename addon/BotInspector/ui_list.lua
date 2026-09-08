-- Bot Inspector -- left pane: the zone tree.
--
-- A flat list of zones was fine while the panel only had to prove the protocol worked. With two
-- hundred bots spread over forty-odd zones it ran off the bottom of the window, so this is a
-- scrolling tree: zone headers that expand in place, with the bots underneath them.

local BI = BotInspector
local W  = BI.W
local UI = BI.UI

local ROW_H       = 15
local VISIBLE     = math.floor((UI.left:GetHeight() - 40) / ROW_H)
local SORT_CYCLE  = { "level", "name", "class" }

local expanded = {}          -- zoneId -> true
local items    = {}          -- flattened tree, rebuilt on every draw
local rows     = {}
local findResults, findMatched = nil, 0

local function settings()
    return BotInspectorDB or UI.defaults
end

-- ---------------------------------------------------------------------------------------------
-- Sort control
-- ---------------------------------------------------------------------------------------------

local sortButton = CreateFrame("Button", nil, UI.left, "UIPanelButtonTemplate")
sortButton:SetWidth(110); sortButton:SetHeight(22)
sortButton:SetPoint("TOPLEFT", 6, -5)

local countLabel = UI.left:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
countLabel:SetPoint("TOPRIGHT", -8, -11)

local draw   -- forward declared: the handlers below run before it is defined

local function sortLabel()
    local s = settings()
    return string.format("Sort: %s %s", s.sort, s.descending and "v" or "^")
end

sortButton:SetScript("OnClick", function()
    local s = settings()
    -- One control, two axes: clicking cycles the field and flips direction on the way round, so the
    -- descending and ascending forms of each field are both reachable without a second widget.
    if s.descending then
        s.descending = false
    else
        s.descending = true
        local i = 1
        for n, field in ipairs(SORT_CYCLE) do if field == s.sort then i = n end end
        s.sort = SORT_CYCLE[(i % #SORT_CYCLE) + 1]
    end
    sortButton:SetText(sortLabel())
    draw()
end)

-- ---------------------------------------------------------------------------------------------
-- Tree construction
-- ---------------------------------------------------------------------------------------------

local function botSortValue(b)
    local s = settings()
    if s.sort == "name"  then return (b.name or ""):lower() end
    if s.sort == "class" then return (W.CLASS_NAME[b.class] or "?"):lower() end
    return b.level or 0
end

local function sortBots(list)
    local s = settings()
    table.sort(list, function(a, b)
        local av, bv = botSortValue(a), botSortValue(b)
        if av == bv then return (a.name or "") < (b.name or "") end
        if s.descending then return av > bv end
        return av < bv
    end)
end

--- Does this bot match the search box?
-- Name, class and level all count, because "show me the level 80 druids" is a question that gets
-- asked while debugging and none of those three alone answers it.
local function botMatches(b, needle)
    if (b.name or ""):lower():find(needle, 1, true) then return true end
    if (W.CLASS_NAME[b.class] or ""):lower():find(needle, 1, true) then return true end
    if tostring(b.level or 0) == needle then return true end
    return false
end

local function build()
    items = {}

    if UI.viewMode == "find" then
        for _, b in ipairs(findResults or {}) do
            table.insert(items, { kind = "bot", bot = b, zone = b.zone })
        end
        return
    end

    local needle = UI.search ~= "" and UI.search:lower() or nil

    local zones = {}
    for zoneId, count in pairs(BI.zones) do
        table.insert(zones, { id = zoneId, n = count, name = BI.zoneNames[zoneId] or ("zone " .. zoneId) })
    end
    table.sort(zones, function(a, b)
        if a.n == b.n then return a.name < b.name end
        return a.n > b.n
    end)

    for _, z in ipairs(zones) do
        local roster    = BI.roster[z.id]
        local zoneHit   = needle and z.name:lower():find(needle, 1, true)
        local matched   = nil

        if needle and roster then
            matched = {}
            for _, b in ipairs(roster) do
                if botMatches(b, needle) then table.insert(matched, b) end
            end
            if #matched == 0 then matched = nil end
        end

        -- With a search active, show a zone only if it or one of its loaded bots matches, and
        -- auto-expand the ones that matched on a bot so the hit is visible without another click.
        if not needle or zoneHit or matched then
            table.insert(items, { kind = "zone", zone = z })

            local show = matched or (expanded[z.id] and roster)
            if matched or expanded[z.id] then
                if show then
                    local list = {}
                    for _, b in ipairs(show) do table.insert(list, b) end
                    sortBots(list)
                    for _, b in ipairs(list) do
                        table.insert(items, { kind = "bot", bot = b, zone = z.id })
                    end
                elseif expanded[z.id] then
                    table.insert(items, { kind = "note", text = "loading..." })
                end
            end
        end
    end
end

-- ---------------------------------------------------------------------------------------------
-- Rows
-- ---------------------------------------------------------------------------------------------

for i = 1, VISIBLE do
    local r = W.Row(UI.left, UI.LEFT_W - 12, ROW_H)
    r:SetPoint("TOPLEFT", 6, -33 - (i - 1) * ROW_H)
    rows[i] = r
end

local function toggleZone(zoneId)
    if expanded[zoneId] then
        expanded[zoneId] = nil
    else
        expanded[zoneId] = true
        if not BI.roster[zoneId] then
            UI:SetStatus("requesting roster for %s...", BI.zoneNames[zoneId] or ("zone " .. zoneId))
            BI:RequestList(zoneId)
        end
    end
    draw()
end

function draw()
    build()

    local offset = UI.left.offset or 0
    if offset > math.max(0, #items - VISIBLE) then
        offset = math.max(0, #items - VISIBLE)
        UI.left.offset = offset
    end

    for i = 1, VISIBLE do
        local row  = rows[i]
        local item = items[i + offset]

        row.link = nil
        row.right:SetText("")

        if not item then
            row.text:SetText("")
            row:SetScript("OnClick", nil)
            row:Hide()
        else
            row:Show()
            if item.kind == "zone" then
                local z = item.zone
                W.SetRowClassIcon(row, nil, 4)   -- rows are recycled; clear a previous bot's icon
                row.text:SetText(string.format("%s %s", expanded[z.id] and "-" or "+", z.name))
                row.text:SetTextColor(1, 0.82, 0)
                row.right:SetText(tostring(z.n))
                row:SetScript("OnClick", function() toggleZone(z.id) end)

            elseif item.kind == "bot" then
                local b = item.bot
                local r, g, bl = W.ClassColor(b.class)
                W.SetRowClassIcon(row, b.class, 16)
                row.text:SetText(b.name or "?")
                row.text:SetTextColor(r, g, bl)
                row.right:SetText(tostring(b.level or 0))
                row:SetScript("OnClick", function() UI:SelectBot(b.guid, b) end)

            else
                W.SetRowClassIcon(row, nil, 16)
                row.text:SetText(item.text)
                row.text:SetTextColor(0.6, 0.6, 0.6)
                row:SetScript("OnClick", nil)
            end
        end
    end

    countLabel:SetText(#items > VISIBLE
        and string.format("%d-%d/%d", offset + 1, math.min(offset + VISIBLE, #items), #items)
        or tostring(#items))
end

UI.DrawTree = draw

W.MakeScrollable(UI.left, function() return #items end, function() return VISIBLE end, draw)

-- ---------------------------------------------------------------------------------------------
-- Search wiring
-- ---------------------------------------------------------------------------------------------

UI.search_box:SetScript("OnTextChanged", function(self)
    UI.search = self:GetText() or ""
    if UI.viewMode == "find" and UI.search == "" then
        UI.viewMode = "tree"
    end
    UI.left.offset = 0
    draw()
end)

UI.search_box:SetScript("OnEnterPressed", function(self)
    local text = (self:GetText() or ""):gsub("^%s+", ""):gsub("%s+$", "")
    if #text < 3 then
        -- The server refuses shorter substrings anyway; saying so beats a silent no-op.
        UI:SetStatus("search needs at least 3 characters (a filter on loaded zones still applies)")
        return
    end
    UI:SetStatus("searching all bots for '%s'...", text)
    BI:RequestFind(text)
    self:ClearFocus()
end)

UI.search_box:SetScript("OnEscapePressed", function(self)
    self:SetText("")
    self:ClearFocus()
    UI.viewMode = "tree"
    draw()
end)

-- ---------------------------------------------------------------------------------------------
-- Handlers
-- ---------------------------------------------------------------------------------------------

BI:SetHandler("OnZones", function()
    local n = 0
    for _ in pairs(BI.zones) do n = n + 1 end
    UI:SetStatus("%d zones with bots", n)
    draw()
end)

BI:SetHandler("OnList", function(zoneId, list)
    expanded[zoneId] = true
    UI:SetStatus("%s: %d bots", BI.zoneNames[zoneId] or ("zone " .. zoneId), #list)
    draw()
end)

BI:SetHandler("OnFind", function(list, matched)
    findResults, findMatched = list, matched
    UI.viewMode = "find"
    UI.left.offset = 0
    UI:SetStatus("%d of %d matches -- Esc clears", #list, matched)
    draw()
end)

UI.OnSettingsLoaded = function()
    sortButton:SetText(sortLabel())
    draw()
end
sortButton:SetText(sortLabel())
