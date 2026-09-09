-- Bot Inspector -- frame shell.
--
-- Builds the window and the pieces both panes share, and owns the state they agree on: which bot is
-- selected, which section is showing, what the search box holds. ui_list.lua draws the left pane,
-- ui_detail.lua the right.

local BI = BotInspector
local W  = BI.W

local FRAME_W, FRAME_H = 720, 560
local LEFT_X, LEFT_W   = 14, 226
local RIGHT_X, RIGHT_W = 248, 458

BI.UI = BI.UI or {}
local UI = BI.UI

UI.selectedZone   = nil
UI.selectedBot    = nil
UI.activeSection  = "CORE"
UI.viewMode       = "tree"      -- "tree" | "find"
UI.search         = ""

local frame = CreateFrame("Frame", "BotInspectorFrame", UIParent)
frame:SetWidth(FRAME_W); frame:SetHeight(FRAME_H)
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
frame:SetClampedToScreen(true)
frame:Hide()
UI.frame = frame

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOPLEFT", 20, -16)
title:SetText("Bot Inspector")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -8, -8)

-- Search. Typing filters what is already loaded; Enter asks the server, because the tree only holds
-- the zones that have actually been expanded and a client-side filter cannot see the rest.
local search, hasTemplate = W.Create("EditBox", "BotInspectorSearch", frame, "InputBoxTemplate")
search:SetWidth(150); search:SetHeight(22)
search:SetPoint("TOPRIGHT", -34, -14)
search:SetAutoFocus(false)
search:SetFontObject("GameFontHighlightSmall")
search:SetMaxLetters(24)
if not hasTemplate then
    search:SetBackdrop(W.BACKDROP_PANEL)
    search:SetBackdropColor(0, 0, 0, 0.6)
    search:SetTextInsets(6, 6, 0, 0)
end
UI.search_box = search

local searchHint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
searchHint:SetPoint("RIGHT", search, "LEFT", -6, 0)
searchHint:SetText("search")

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
status:SetPoint("BOTTOMLEFT", 22, 20)
status:SetWidth(520); status:SetJustifyH("LEFT")
UI.status = status

function UI:SetStatus(fmt, ...)
    status:SetText(select("#", ...) > 0 and string.format(fmt, ...) or fmt)
end

local refresh = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
refresh:SetWidth(80); refresh:SetHeight(24)
refresh:SetPoint("BOTTOMRIGHT", -18, 16)
refresh:SetText("Refresh")
UI.refresh = refresh

-- Altbot Controls sits beside Refresh and toggles a full-window page (ui_alts.lua). Wider than the
-- other buttons because "Altbot Controls" does not fit the standard 80, and it doubles as "Back".
local altsButton = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
altsButton:SetWidth(120); altsButton:SetHeight(24)
altsButton:SetPoint("RIGHT", refresh, "LEFT", -6, 0)
altsButton:SetText("Altbot Controls")
altsButton:SetScript("OnClick", function()
    if UI.ShowAlts then UI:ShowAlts(not UI.altsShown) end
end)
UI.altsButton = altsButton

-- Panes
local left = W.Panel(frame, 0, 0, 0, 0.30)
left:SetPoint("TOPLEFT", LEFT_X + 8, -44)
left:SetWidth(LEFT_W); left:SetHeight(FRAME_H - 92)
UI.left, UI.LEFT_W = left, LEFT_W

local right = W.Panel(frame, 0, 0, 0, 0.20)
right:SetPoint("TOPLEFT", RIGHT_X, -44)
right:SetWidth(RIGHT_W); right:SetHeight(FRAME_H - 92)
UI.right, UI.RIGHT_W = right, RIGHT_W

-- Saved settings. Written on load by ADDON_LOADED; until then the defaults below apply.
UI.defaults = { sort = "level", descending = true }

local loader = CreateFrame("Frame")
loader:RegisterEvent("ADDON_LOADED")
loader:SetScript("OnEvent", function(self, event, name)
    if name ~= "BotInspector" then return end
    BotInspectorDB = BotInspectorDB or {}
    for k, v in pairs(UI.defaults) do
        if BotInspectorDB[k] == nil then BotInspectorDB[k] = v end
    end
    if UI.OnSettingsLoaded then UI.OnSettingsLoaded() end
end)

function BI:Toggle()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
        -- Always reopen on the roster. The alts page is a detour, not a mode.
        if UI.ShowAlts and UI.altsShown then UI:ShowAlts(false) end
        UI:SetStatus("requesting zones...")
        self:RequestZones()
    end
end
