-- Bot Inspector -- A2 interface.
--
-- Deliberately plain. A2's job is to prove the protocol end to end: zones arrive, a zone's roster
-- arrives, a bot's detail arrives. Styling is A4, and doing it now would only make a broken
-- transport harder to see.

local BI = BotInspector

local ROW_HEIGHT, MAX_ROWS = 14, 24
local selectedZone, selectedBot

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
status:SetText("")

local refresh = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
refresh:SetWidth(80); refresh:SetHeight(20)
refresh:SetPoint("BOTTOMRIGHT", -18, 14)
refresh:SetText("Refresh")
refresh:SetScript("OnClick", function()
    status:SetText("requesting zones...")
    BI:RequestZones()
end)

-- Left: zones. Right: the selected zone's bots, then the selected bot's detail.
local leftRows, rightRows = {}, {}

local function makeRow(parent, index, xOffset, width)
    local btn = CreateFrame("Button", nil, parent)
    btn:SetWidth(width); btn:SetHeight(ROW_HEIGHT)
    btn:SetPoint("TOPLEFT", xOffset, -40 - (index - 1) * ROW_HEIGHT)
    local text = btn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    text:SetPoint("LEFT", 4, 0)
    text:SetJustifyH("LEFT")
    btn.text = text
    btn:SetScript("OnEnter", function(self) self.text:SetTextColor(1, 1, 0) end)
    btn:SetScript("OnLeave", function(self) self.text:SetTextColor(1, 1, 1) end)
    return btn
end

for i = 1, MAX_ROWS do
    leftRows[i]  = makeRow(frame, i, 22, 180)
    rightRows[i] = makeRow(frame, i, 210, 330)
end

local function clear(rows)
    for _, r in ipairs(rows) do r.text:SetText(""); r:SetScript("OnClick", nil) end
end

local function drawZones()
    clear(leftRows)
    local ordered = {}
    for zoneId, count in pairs(BI.zones) do table.insert(ordered, { id = zoneId, n = count }) end
    table.sort(ordered, function(a, b) return a.n > b.n end)

    for i = 1, math.min(#ordered, MAX_ROWS) do
        local z = ordered[i]
        leftRows[i].text:SetText(string.format("zone %d  (%d)", z.id, z.n))
        leftRows[i]:SetScript("OnClick", function()
            selectedZone = z.id
            status:SetText("requesting zone " .. z.id .. "...")
            BI:RequestList(z.id)
        end)
    end
    status:SetText(string.format("%d zones with bots", #ordered))
end

local function drawRoster(zoneId, list)
    clear(rightRows)
    for i = 1, math.min(#list, MAX_ROWS) do
        local b = list[i]
        rightRows[i].text:SetText(string.format("%s  lvl %d  %dg", b.name, b.level, b.gold or 0))
        rightRows[i]:SetScript("OnClick", function()
            selectedBot = b.guid
            status:SetText("requesting detail for " .. b.name .. "...")
            for _, section in ipairs({ "CORE", "GEAR", "SKILL", "QUEST" }) do
                BI:RequestDetail(b.guid, section)
            end
        end)
    end
    status:SetText(string.format("zone %d: %d bots", zoneId, #list))
end

local function drawDetail(guid, section, rows)
    -- A2 shows raw rows. Making sense of them is A3's job; seeing them arrive is this one's.
    if guid ~= selectedBot or section ~= "CORE" then return end
    clear(rightRows)
    for i = 1, math.min(#rows, MAX_ROWS) do
        rightRows[i].text:SetText(rows[i])
        rightRows[i]:SetScript("OnClick", nil)
    end
    local d = BI.detail[guid]
    status:SetText(string.format("bot %d  CORE %d rows  (fetched %ds ago)",
                                 guid, #rows, math.floor(GetTime() - (d and d.fetchedAt or GetTime()))))
end

BI:SetHandler("OnZones",  function() drawZones() end)
BI:SetHandler("OnList",   function(zoneId, list) drawRoster(zoneId, list) end)
BI:SetHandler("OnDetail", function(guid, section, rows) drawDetail(guid, section, rows) end)
BI:SetHandler("OnFind",   function(list, matched)
    clear(rightRows)
    for i = 1, math.min(#list, MAX_ROWS) do
        local b = list[i]
        rightRows[i].text:SetText(string.format("%s  lvl %d  zone %d", b.name, b.level, b.zone))
        rightRows[i]:SetScript("OnClick", function()
            selectedBot = b.guid
            BI:RequestDetail(b.guid, "CORE")
        end)
    end
    status:SetText(string.format("showing %d of %d matches", #list, matched))
end)
BI:SetHandler("OnError",  function(_, text) status:SetText("error: " .. text) end)

function BI:Toggle()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
        status:SetText("requesting zones...")
        self:RequestZones()
    end
end
