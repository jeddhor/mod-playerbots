-- Bot Inspector -- a small always-on display of what a self bot is actually doing.
--
-- Watching your own character walk off and having no idea whether it is heading for a gather node,
-- a quest giver or a grind spot -- and no way to tell a long walk from a stuck one -- is most of
-- what makes self-bot behaviour hard to reason about. Every field here already exists on the
-- server; none of it was reachable from inside the game.
--
-- Deliberately small and deliberately separate from the main window: it is meant to be left open
-- while playing, which the inspector panel is not.

local BI = BotInspector
local UI = BI.UI
local W  = BI.W

local POLL_SECONDS = 2.0
local WIDTH, LINE_HEIGHT, PAD = 230, 13, 8

-- Fields in display order. Each returns a string, or nil to omit the line entirely -- a display
-- that hides what does not apply stays readable, where one full of "n/a" does not.
local FIELDS = {
    {
        label = "Doing",
        get = function(s) return s.act end,
    },
    {
        label = "For",
        get = function(s)
            local secs = tonumber(s.secs)
            if not secs then return nil end
            if secs < 60 then return secs .. "s" end
            return string.format("%dm %ds", math.floor(secs / 60), secs % 60)
        end,
    },
    {
        label = "Action",
        get = function(s) return s.action end,
    },
    {
        label = "Heading",
        get = function(s)
            if not s.destDist then return nil end
            local where = BI.zoneNames and BI.zoneNames[s.destArea] or nil
            local climb = s.destClimb or 0
            -- Climb is shown only when it is worth walking about: a couple of yards of undulation
            -- is noise, and printing it would bury the cases that matter.
            local rise = (math.abs(climb) >= 10) and string.format(", %+d up", climb) or ""
            if where then
                return string.format("%s, %d yd%s", where, s.destDist, rise)
            end
            return string.format("%d yd%s", s.destDist, rise)
        end,
    },
    {
        label = "Moving",
        get = function(s)
            if s.human then return "no -- you are steering" end
            return s.move and "yes" or "no"
        end,
    },
    {
        label = "Stuck",
        -- Five of these teleports the character. Until this existed the only visible symptom was
        -- suddenly being somewhere else.
        get = function(s)
            local n = tonumber(s.stuck) or 0
            if n == 0 then return nil end
            return string.format("%d/5 attempts", n)
        end,
    },
    {
        label = "Route",
        get = function(s)
            if not s.routeIndex then return nil end
            return string.format("node %d, %d visited", s.routeIndex, s.routeVisited or 0)
        end,
    },
    {
        label = "Target",
        get = function(s)
            if not s.targetName then return nil end
            return string.format("%s (%d) %d%%", s.targetName, s.targetLevel or 0, s.targetHp or 0)
        end,
    },
}

-- ---- frame ------------------------------------------------------------------------------------

local frame = W.Create("Frame", "BotInspectorSelfStat", UIParent)
frame:SetWidth(WIDTH)
frame:SetHeight(120)
frame:SetFrameStrata("MEDIUM")
frame:SetClampedToScreen(true)
frame:SetMovable(true)
frame:EnableMouse(true)
frame:RegisterForDrag("LeftButton")
frame:Hide()
UI.selfStatFrame = frame

frame:SetBackdrop({
    bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
frame:SetBackdropColor(0, 0, 0, 0.75)

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
title:SetPoint("TOPLEFT", PAD, -PAD)
title:SetText("Self Bot")

--- The pin. Pinned means the frame ignores the mouse entirely: it stops being draggable *and*
-- stops swallowing clicks, so it can sit over the world without getting in the way.
local pin = W.Create("Button", nil, frame)
pin:SetWidth(16); pin:SetHeight(16)
pin:SetPoint("TOPRIGHT", -PAD + 2, -PAD + 2)
pin:SetNormalTexture("Interface\\Buttons\\UI-AutoCastableOverlay")
pin:GetNormalTexture():SetTexCoord(0.2, 0.8, 0.2, 0.8)

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
status:SetPoint("TOPLEFT", PAD, -PAD - 15)
status:SetWidth(WIDTH - PAD * 2)
status:SetJustifyH("LEFT")

-- One label/value pair per field. Built once; hidden rows collapse so the frame has no gaps.
local lines = {}
for i = 1, #FIELDS do
    local row = {}
    row.label = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    row.label:SetWidth(52)
    row.label:SetJustifyH("LEFT")
    row.value = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.value:SetWidth(WIDTH - PAD * 2 - 56)
    row.value:SetJustifyH("LEFT")
    lines[i] = row
end

-- ---- persistence ------------------------------------------------------------------------------

local function db()
    BotInspectorDB = BotInspectorDB or {}
    return BotInspectorDB
end

local function savePosition()
    local point, _, relPoint, x, y = frame:GetPoint()
    if not point then return end
    local d = db()
    d.selfStatPos = { point = point, relPoint = relPoint, x = x, y = y }
end

local function restorePosition()
    local pos = db().selfStatPos
    frame:ClearAllPoints()
    if pos and pos.point then
        frame:SetPoint(pos.point, UIParent, pos.relPoint or pos.point, pos.x or 0, pos.y or 0)
    else
        frame:SetPoint("CENTER", UIParent, "CENTER", 260, 0)
    end
end

-- ---- pinning ----------------------------------------------------------------------------------

local function applyPinned()
    local pinned = db().selfStatPinned and true or false

    -- EnableMouse(false) is what makes a pinned frame click-through. Without it the display would
    -- still eat clicks aimed at the world behind it, which is worse than it being draggable.
    frame:EnableMouse(not pinned)
    frame:SetMovable(not pinned)

    if pinned then
        pin:GetNormalTexture():SetVertexColor(0.2, 1.0, 0.2)
    else
        pin:GetNormalTexture():SetVertexColor(0.7, 0.7, 0.7)
    end
end

pin:SetScript("OnClick", function()
    local d = db()
    d.selfStatPinned = not d.selfStatPinned
    applyPinned()
end)

pin:SetScript("OnEnter", function()
    GameTooltip:SetOwner(pin, "ANCHOR_LEFT")
    if db().selfStatPinned then
        GameTooltip:SetText("Pinned -- click to unlock and move")
    else
        GameTooltip:SetText("Click to pin in place")
    end
    GameTooltip:Show()
end)
pin:SetScript("OnLeave", function() GameTooltip:Hide() end)

frame:SetScript("OnDragStart", function(self)
    if not db().selfStatPinned then self:StartMoving() end
end)
frame:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    savePosition()
end)

-- ---- drawing ----------------------------------------------------------------------------------

local function redraw()
    local stat = BI.selfStat

    if not stat then
        status:SetText("Waiting for the server...")
    elseif stat.off then
        status:SetText("Self-bot mode is off.")
    else
        status:SetText("")
    end

    local y = -PAD - 15
    if status:GetText() ~= "" then
        y = y - LINE_HEIGHT
    end

    local shown = 0
    for i, field in ipairs(FIELDS) do
        local row = lines[i]
        local text = (stat and not stat.off) and field.get(stat) or nil

        if text then
            row.label:ClearAllPoints()
            row.value:ClearAllPoints()
            row.label:SetPoint("TOPLEFT", PAD, y)
            row.value:SetPoint("TOPLEFT", PAD + 56, y)
            row.label:SetText(field.label)
            row.value:SetText(text)
            row.label:Show(); row.value:Show()
            y = y - LINE_HEIGHT
            shown = shown + 1
        else
            row.label:Hide(); row.value:Hide()
        end
    end

    frame:SetHeight(math.max(48, -y + PAD))
end

BI:SetHandler("OnSelfStat", function() redraw() end)

-- ---- polling ----------------------------------------------------------------------------------

local elapsed = 0
frame:SetScript("OnUpdate", function(self, delta)
    elapsed = elapsed + (delta or 0)
    if elapsed < POLL_SECONDS then return end
    elapsed = 0
    BI:RequestSelfStat()
end)

-- ---- public -----------------------------------------------------------------------------------

--- Show or hide the display, remembering the choice across sessions.
function UI.SetSelfStatShown(shown)
    db().selfStatShown = shown and true or false
    if shown then
        restorePosition()
        applyPinned()
        redraw()
        frame:Show()
        -- Ask immediately rather than waiting out the first poll, so ticking the box fills the
        -- display straight away.
        BI:RequestSelfStat()
    else
        frame:Hide()
    end
end

function UI.IsSelfStatShown()
    return db().selfStatShown and true or false
end

--- Restore on login.
local loader = W.Create("Frame", nil, UIParent)
loader:RegisterEvent("PLAYER_LOGIN")
loader:SetScript("OnEvent", function()
    if UI.IsSelfStatShown() then UI.SetSelfStatShown(true) end
end)
