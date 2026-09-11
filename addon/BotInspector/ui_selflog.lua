-- Bot Inspector -- a scrolling record of what your self bot has been doing.
--
-- The stats window answers "what is it doing now". This answers "what has it been doing", which is
-- the question that actually comes up: an item you wanted was sold, gold went somewhere, the
-- character died while you were looking at something else. None of that is recoverable afterwards
-- from the client -- the combat log has rolled over and the server log is half a gigabyte of other
-- people's bots -- so the server keeps a short per-character ring and this displays it.

local BI = BotInspector
local UI = BI.UI
local W  = BI.W

local POLL_SECONDS = 2.0
local PAD = 8
local DEFAULT_WIDTH, MIN_WIDTH, MAX_WIDTH = 420, 260, 900
local DEFAULT_HEIGHT, MIN_HEIGHT, MAX_HEIGHT = 260, 120, 800
local LINE_HEIGHT = 12

-- Category -> label and colour. The letter is what comes over the wire; keeping the mapping here
-- means the server can add a category without the client breaking, it just shows up uncoloured
-- under its own letter.
local CATS = {
    S = { name = "Sell",    r = 0.85, g = 0.75, b = 0.40 },
    B = { name = "Buy",     r = 0.80, g = 0.60, b = 0.35 },
    A = { name = "Auction", r = 0.55, g = 0.80, b = 0.95 },
    U = { name = "Use",     r = 0.70, g = 0.85, b = 0.60 },
    L = { name = "Loot",    r = 0.60, g = 0.90, b = 0.60 },
    Q = { name = "Quest",   r = 1.00, g = 0.85, b = 0.30 },
    M = { name = "Mail",    r = 0.75, g = 0.75, b = 0.95 },
    G = { name = "Gold",    r = 1.00, g = 0.95, b = 0.55 },
    D = { name = "Death",   r = 1.00, g = 0.45, b = 0.45 },
    N = { name = "Note",    r = 0.75, g = 0.75, b = 0.75 },
}

--- Copper as g/s/c, dropping the units that would read as zero.
-- Written out rather than using GetCoinTextureString so the line stays narrow: coin icons in a
-- dense log cost a surprising amount of width for something the reader can already infer.
local function money(copper)
    if not copper or copper == 0 then return "" end
    local neg = copper < 0
    local c = math.abs(copper)
    local g = math.floor(c / 10000)
    local s = math.floor((c % 10000) / 100)
    local cc = c % 100
    local out
    if g > 0 then
        out = string.format("%dg%02ds", g, s)
    elseif s > 0 then
        out = string.format("%ds%02dc", s, cc)
    else
        out = string.format("%dc", cc)
    end
    return (neg and "-" or "+") .. out
end

local function clock(when)
    if not when or when == 0 then return "--:--" end
    return date("%H:%M:%S", when)
end

-- ---- frame ------------------------------------------------------------------------------------

local frame = W.Create("Frame", "BotInspectorSelfLog", UIParent)
frame:SetWidth(DEFAULT_WIDTH)
frame:SetHeight(DEFAULT_HEIGHT)
frame:SetFrameStrata("MEDIUM")
frame:SetClampedToScreen(true)
frame:SetMovable(true)
frame:EnableMouse(true)
frame:RegisterForDrag("LeftButton")
frame:SetResizable(true)
frame:SetMinResize(MIN_WIDTH, MIN_HEIGHT)
frame:SetMaxResize(MAX_WIDTH, MAX_HEIGHT)
frame:Hide()
UI.selfLogFrame = frame

frame:SetBackdrop({
    bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
frame:SetBackdropColor(0, 0, 0, 0.80)

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
title:SetPoint("TOPLEFT", PAD, -PAD)
title:SetText("Self Bot Log")

local summary = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
summary:SetPoint("TOPRIGHT", -PAD - 18, -PAD)
summary:SetJustifyH("RIGHT")

--- Pinned means the frame ignores the mouse entirely, so it can sit over the world without
-- swallowing clicks meant for the game.
local pin = W.Create("Button", nil, frame)
pin:SetWidth(16); pin:SetHeight(16)
pin:SetPoint("TOPRIGHT", -PAD + 2, -PAD + 2)
pin:SetNormalTexture("Interface\\Buttons\\UI-AutoCastableOverlay")
pin:GetNormalTexture():SetTexCoord(0.2, 0.8, 0.2, 0.8)

-- Scrolling message frame rather than a table of font strings.
--
-- It already does the two things this needs and neither is trivial to get right by hand: it keeps a
-- bounded backlog with no per-line frames, and it holds scroll position when lines are added at the
-- bottom, so reading back through history is not interrupted every two seconds by a new event.
local log = CreateFrame("ScrollingMessageFrame", "BotInspectorSelfLogText", frame)
log:SetPoint("TOPLEFT", PAD, -PAD - 16)
log:SetPoint("BOTTOMRIGHT", -PAD, PAD + 4)
log:SetFontObject(GameFontHighlightSmall)
log:SetJustifyH("LEFT")
log:SetFading(false)
log:SetMaxLines(500)
log:EnableMouseWheel(true)
log:SetScript("OnMouseWheel", function(self, delta)
    if delta > 0 then self:ScrollUp() else self:ScrollDown() end
end)

-- Exposed so the offline harness can read back what was actually drawn. Asserting that redraw did
-- not throw is a much weaker test than asserting the lines it produced, and the difference has
-- already caught a real counting bug in this addon once.
UI.selfLogText = log

-- A grab strip along the bottom-right corner.
local grip = W.Create("Frame", nil, frame)
grip:SetWidth(14); grip:SetHeight(14)
grip:SetPoint("BOTTOMRIGHT", -2, 2)
grip:EnableMouse(true)

local gripTex = grip:CreateTexture(nil, "OVERLAY")
gripTex:SetAllPoints(grip)
gripTex:SetTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")

-- ---- saved state ------------------------------------------------------------------------------

local function db()
    BotInspectorDB = BotInspectorDB or {}
    return BotInspectorDB
end

local function saveGeometry()
    local d = db()
    d.selfLogWidth  = math.floor(frame:GetWidth() + 0.5)
    d.selfLogHeight = math.floor(frame:GetHeight() + 0.5)
end

local function restoreGeometry()
    local d = db()
    frame:SetWidth(math.max(MIN_WIDTH, math.min(MAX_WIDTH, d.selfLogWidth or DEFAULT_WIDTH)))
    frame:SetHeight(math.max(MIN_HEIGHT, math.min(MAX_HEIGHT, d.selfLogHeight or DEFAULT_HEIGHT)))
end

local function savePosition()
    local point, _, relPoint, x, y = frame:GetPoint()
    local d = db()
    d.selfLogPos = { point = point, relPoint = relPoint, x = x, y = y }
end

local function restorePosition()
    local p = db().selfLogPos
    frame:ClearAllPoints()
    if p and p.point then
        frame:SetPoint(p.point, UIParent, p.relPoint, p.x, p.y)
    else
        -- Below the stats window by default, so the two read as a pair rather than landing on top
        -- of each other the first time both are opened.
        frame:SetPoint("CENTER", UIParent, "CENTER", 260, -120)
    end
end

local function applyPinned()
    local pinned = db().selfLogPinned and true or false
    frame:EnableMouse(not pinned)
    frame:SetMovable(not pinned)
    if pinned then grip:Hide() else grip:Show() end
    pin:GetNormalTexture():SetVertexColor(pinned and 1 or 0.5, pinned and 0.82 or 0.5,
                                          pinned and 0 or 0.5)
end

-- ---- drawing ----------------------------------------------------------------------------------

--- Rebuild the whole view from BI.selfLog.
--
-- Cheap enough at a few hundred lines, and it means there is exactly one path that produces the
-- display rather than an append path and a rebuild path that can disagree about what is shown.
--- "rrggbb" for a category colour.
--
-- Floored explicitly: %02x on a float works in 5.1 by truncating, but relying on that is the kind
-- of thing that quietly changes meaning under a different Lua.
local function hex(cat)
    return string.format("%02x%02x%02x", math.floor(cat.r * 255), math.floor(cat.g * 255),
                         math.floor(cat.b * 255))
end

local function redraw()
    log:Clear()

    local events = BI.selfLog or {}
    for _, ev in ipairs(events) do
        local cat = CATS[ev.cat] or CATS.N
        local coin = money(ev.delta)
        local line
        if coin ~= "" then
            line = string.format("|cff808080%s|r |cff%s%s|r %s  |cffffd700%s|r",
                                 clock(ev.when), hex(cat), cat.name, ev.text or "", coin)
        else
            line = string.format("|cff808080%s|r |cff%s%s|r %s",
                                 clock(ev.when), hex(cat), cat.name, ev.text or "")
        end
        log:AddMessage(line)
    end

    local n = #events
    local gold = BI.selfStat and BI.selfStat.gold
    if n == 0 then
        summary:SetText("no events yet")
    elseif gold then
        summary:SetText(string.format("%d events  |cffffd700%dg|r", n, gold))
    else
        summary:SetText(string.format("%d events", n))
    end
end

-- ---- interaction ------------------------------------------------------------------------------

frame:SetScript("OnDragStart", function(self) if self:IsMovable() then self:StartMoving() end end)
frame:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    savePosition()
end)

grip:SetScript("OnMouseDown", function() frame:StartSizing("BOTTOMRIGHT") end)
grip:SetScript("OnMouseUp", function()
    frame:StopMovingOrSizing()
    saveGeometry()
    redraw()
end)

pin:SetScript("OnClick", function()
    local d = db()
    d.selfLogPinned = not d.selfLogPinned
    applyPinned()
end)
pin:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText(db().selfLogPinned and "Unpin (allow dragging)" or "Pin in place")
    GameTooltip:Show()
end)
pin:SetScript("OnLeave", function() GameTooltip:Hide() end)

BI:SetHandler("OnSelfLog", function() if frame:IsShown() then redraw() end end)

-- ---- polling ----------------------------------------------------------------------------------

local elapsed = 0
frame:SetScript("OnUpdate", function(self, delta)
    elapsed = elapsed + delta
    if elapsed < POLL_SECONDS then return end
    elapsed = 0

    if BI.RequestSelfLog then BI:RequestSelfLog() end
end)

function UI.SetSelfLogShown(shown)
    if shown then
        restoreGeometry()
        restorePosition()
        applyPinned()
        frame:Show()
        redraw()
        -- Ask immediately rather than waiting out the first poll interval, so opening the window
        -- shows the backlog at once instead of an empty box for two seconds.
        if BI.RequestSelfLog then BI:RequestSelfLog() end
    else
        frame:Hide()
    end
    db().selfLogShown = shown and true or false
end

function UI.IsSelfLogShown()
    return frame:IsShown()
end

-- Restore visibility across a reload, the same way the stats window does.
local loader = W.Create("Frame", nil, UIParent)
loader:RegisterEvent("PLAYER_ENTERING_WORLD")
loader:SetScript("OnEvent", function(self)
    self:UnregisterEvent("PLAYER_ENTERING_WORLD")
    if db().selfLogShown then UI.SetSelfLogShown(true) end
end)
