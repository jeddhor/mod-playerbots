-- Bot Inspector -- minimap button.
--
-- Opens the panel without typing /bi. Draggable around the minimap ring, and its position is saved,
-- because a button that lands on top of somebody's clock or tracking icon is worse than no button.
--
-- Every texture here was checked against the client's own MPQ listfiles rather than remembered:
-- a wrong path shows as a solid green square with no error, which is a silent failure and exactly
-- the kind this addon has been bitten by before. All four live in locale-enUS.MPQ.

local BI = BotInspector
local UI = BI.UI

-- The book from the spellbook frame -- the glyph the Spellbook & Abilities button uses.
local ICON_TEXTURE   = "Interface\\Spellbook\\Spellbook-Icon"
local BORDER_TEXTURE = "Interface\\Minimap\\MiniMap-TrackingBorder"
local HIGHLIGHT      = "Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight"

local DEFAULT_ANGLE = 194     -- lower-left, clear of the default clock and tracking button
local RADIUS        = 80

local button = CreateFrame("Button", "BotInspectorMinimapButton", Minimap)
button:SetWidth(31); button:SetHeight(31)
button:SetFrameStrata("MEDIUM")
button:SetFrameLevel(8)
button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
button:RegisterForDrag("LeftButton")
button:SetMovable(true)
UI.minimapButton = button

local icon = button:CreateTexture(nil, "BACKGROUND")
icon:SetWidth(20); icon:SetHeight(20)
icon:SetTexture(ICON_TEXTURE)
icon:SetPoint("TOPLEFT", 6, -5)

local border = button:CreateTexture(nil, "OVERLAY")
border:SetWidth(53); border:SetHeight(53)
border:SetTexture(BORDER_TEXTURE)
border:SetPoint("TOPLEFT")

button:SetHighlightTexture(HIGHLIGHT)

--- Place the button on the minimap ring at the saved angle.
local function updatePosition()
    local angle = math.rad(tonumber(BotInspectorDB and BotInspectorDB.minimapAngle or nil) or DEFAULT_ANGLE)
    button:SetPoint("TOPLEFT", Minimap, "TOPLEFT",
                    54 - (RADIUS * math.cos(angle)) - 25,
                    (RADIUS * math.sin(angle)) - 54 + 25)
end
UI.UpdateMinimapPosition = updatePosition

-- Dragging. The angle is recomputed from the cursor each frame while held, which is the standard
-- 3.3.5 approach: there is no drag-to-arc helper, and snapping to the ring is what keeps the button
-- from being dropped somewhere it cannot be clicked.
button:SetScript("OnDragStart", function(self)
    self.dragging = true
    self:SetScript("OnUpdate", function()
        local mx, my = Minimap:GetCenter()
        local scale = UIParent:GetEffectiveScale()
        local cx, cy = GetCursorPosition()
        cx, cy = cx / scale, cy / scale

        local angle = math.deg(math.atan2(cy - my, cx - mx))
        if angle < 0 then angle = angle + 360 end

        BotInspectorDB = BotInspectorDB or {}
        BotInspectorDB.minimapAngle = angle
        updatePosition()
    end)
end)

button:SetScript("OnDragStop", function(self)
    self.dragging = false
    self:SetScript("OnUpdate", nil)
end)

button:SetScript("OnClick", function(_, mouseButton)
    if mouseButton == "RightButton" then
        -- Right-click goes straight to the alt controls, which is the page most worth a shortcut:
        -- it is the one used mid-session, repeatedly, while the roster is a browse-once thing.
        if not UI.frame:IsShown() then BI:Toggle() end
        if UI.ShowAlts then UI:ShowAlts(true) end
        return
    end

    BI:Toggle()
end)

button:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:AddLine("Bot Inspector")
    GameTooltip:AddLine("Left-click to open.", 1, 1, 1)
    GameTooltip:AddLine("Right-click for altbot controls.", 1, 1, 1)
    GameTooltip:AddLine("Drag to move around the minimap.", 0.6, 0.6, 0.6)
    GameTooltip:Show()
end)

button:SetScript("OnLeave", function() GameTooltip:Hide() end)

-- Position once settings exist. ADDON_LOADED has already fired for us by the time this file's
-- OnSettingsLoaded hook would run, so both paths are covered: place it now with whatever defaults
-- are in hand, and again when SavedVariables arrive.
updatePosition()

local prevOnSettingsLoaded = UI.OnSettingsLoaded
UI.OnSettingsLoaded = function(...)
    if prevOnSettingsLoaded then prevOnSettingsLoaded(...) end
    if BotInspectorDB and BotInspectorDB.minimapAngle == nil then
        BotInspectorDB.minimapAngle = DEFAULT_ANGLE
    end
    updatePosition()
end
