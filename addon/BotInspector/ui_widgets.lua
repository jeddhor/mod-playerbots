-- Bot Inspector -- shared widgets.
--
-- Everything here is built from CreateFrame primitives and textures that shipped with 3.3.5 rather
-- than from Blizzard's frame templates. That is a deliberate trade. A missing template makes
-- CreateFrame throw at load time, which takes the whole addon down before a single line runs, and I
-- cannot verify a template's presence from outside the client. Primitives cost more code and cannot
-- fail that way. Where a template genuinely earns its keep it is tried under pcall with a fallback.

local BI = BotInspector
BI.W = {}
local W = BI.W

W.BACKDROP_PANEL = {
    bgFile   = "Interface\\ChatFrame\\ChatFrameBackground",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
}

local BACKDROP_SLOT = {
    bgFile   = "Interface\\Buttons\\UI-Quickslot2",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = false, edgeSize = 10,
    insets = { left = 2, right = 2, top = 2, bottom = 2 },
}

local QUESTION_MARK = "Interface\\Icons\\INV_Misc_QuestionMark"

--- Create a frame with a subtle panel background.
function W.Panel(parent, r, g, b, a)
    local f = CreateFrame("Frame", nil, parent)
    f:SetBackdrop(W.BACKDROP_PANEL)
    f:SetBackdropColor(r or 0, g or 0, b or 0, a or 0.35)
    f:SetBackdropBorderColor(0.35, 0.32, 0.28, 1)
    return f
end

--- An equipment slot: icon, quality-coloured border, tooltip.
--
-- The border is the backdrop's own edge tinted with SetBackdropBorderColor rather than a quality
-- texture, because the texture used for that varies across clients and a wrong path renders nothing
-- at all -- silently, which is the failure mode this addon has already been bitten by once.
function W.ItemSlot(parent, size)
    local b = CreateFrame("Button", nil, parent)
    b:SetWidth(size or 36); b:SetHeight(size or 36)
    b:SetBackdrop(BACKDROP_SLOT)
    b:SetBackdropColor(0, 0, 0, 0.7)
    b:SetBackdropBorderColor(0.3, 0.3, 0.3, 1)

    local icon = b:CreateTexture(nil, "ARTWORK")
    icon:SetPoint("TOPLEFT", 3, -3)
    icon:SetPoint("BOTTOMRIGHT", -3, 3)
    icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)   -- trim the icon border art
    b.icon = icon

    local label = b:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    label:SetPoint("BOTTOM", 0, -11)
    b.label = label

    b:SetScript("OnEnter", function(self)
        if self.link then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink(self.link)
            GameTooltip:Show()
        elseif self.emptyText then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetText(self.emptyText, 0.6, 0.6, 0.6)
            GameTooltip:Show()
        end
    end)
    b:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return b
end

--- Fill a slot from an item id. Returns true when the client already had the item cached.
--
-- GetItemInfo returning nil is the normal first answer for an item this client has never seen; the
-- call itself is what asks the server for it. So report the miss and let the caller retry rather
-- than drawing a permanent blank.
function W.SetItemSlot(slot, itemId, enchant, suffix, slotName)
    slot.emptyText = slotName

    if not itemId or itemId == 0 then
        slot.icon:SetTexture(nil)
        slot.link = nil
        slot:SetBackdropBorderColor(0.3, 0.3, 0.3, 1)
        return true
    end

    slot.link = string.format("item:%d:%d:0:0:0:0:%d:0", itemId, enchant or 0, suffix or 0)

    local name, _, quality, _, _, _, _, _, _, texture = GetItemInfo(itemId)
    if not name then
        slot.icon:SetTexture(QUESTION_MARK)
        slot:SetBackdropBorderColor(0.4, 0.4, 0.4, 1)
        return false
    end

    slot.icon:SetTexture(texture or QUESTION_MARK)
    local r, g, b = GetItemQualityColor(quality or 1)
    slot:SetBackdropBorderColor(r, g, b, 1)
    return true
end

--- A labelled progress bar, used for professions.
function W.Bar(parent, width, height)
    local holder = CreateFrame("Frame", nil, parent)
    holder:SetWidth(width); holder:SetHeight(height + 12)

    local label = holder:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    label:SetPoint("TOPLEFT", 0, 0)
    label:SetJustifyH("LEFT")

    local value = holder:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    value:SetPoint("TOPRIGHT", 0, 0)
    value:SetJustifyH("RIGHT")

    local bar = CreateFrame("StatusBar", nil, holder)
    bar:SetPoint("BOTTOMLEFT", 0, 0)
    bar:SetWidth(width); bar:SetHeight(height)
    bar:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
    bar:SetMinMaxValues(0, 1)

    local bg = bar:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints()
    bg:SetTexture("Interface\\TargetingFrame\\UI-StatusBar")
    bg:SetVertexColor(0.15, 0.15, 0.15, 0.8)

    holder.label, holder.value, holder.bar = label, value, bar
    return holder
end

--- A single text row that can be clicked and can carry a tooltip link.
function W.Row(parent, width, height)
    local btn = CreateFrame("Button", nil, parent)
    btn:SetWidth(width); btn:SetHeight(height)

    local hl = btn:CreateTexture(nil, "HIGHLIGHT")
    hl:SetAllPoints()
    hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    hl:SetBlendMode("ADD")
    hl:SetAlpha(0.4)

    local text = btn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    text:SetPoint("LEFT", 4, 0)
    text:SetJustifyH("LEFT")
    btn.text = text

    local right = btn:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    right:SetPoint("RIGHT", -4, 0)
    right:SetJustifyH("RIGHT")
    btn.right = right

    btn:SetScript("OnEnter", function(self)
        if not self.link then return end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetHyperlink(self.link)
        GameTooltip:Show()
    end)
    btn:SetScript("OnLeave", function() GameTooltip:Hide() end)
    return btn
end

--- Scroll a fixed pool of rows over a longer list, driven by the mouse wheel.
--
-- Hand-rolled rather than FauxScrollFrameTemplate for the reason at the top of this file: a template
-- that is absent takes the addon down at load. An offset and a wheel handler are a dozen lines and
-- cannot.
function W.MakeScrollable(frame, getCount, getVisible, redraw)
    frame.offset = 0
    frame:EnableMouseWheel(true)
    frame:SetScript("OnMouseWheel", function(self, delta)
        local maxOffset = math.max(0, getCount() - getVisible())
        local newOffset = math.max(0, math.min(maxOffset, self.offset - delta * 3))
        if newOffset ~= self.offset then
            self.offset = newOffset
            redraw()
        end
    end)
end

--- Class colour from a class id, falling back to white for anything unrecognised.
local CLASS_TOKEN = {
    [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE",  [5] = "PRIEST",
    [6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}
local CLASS_NAME = {
    [1] = "Warrior", [2] = "Paladin", [3] = "Hunter", [4] = "Rogue", [5] = "Priest",
    [6] = "Death Knight", [7] = "Shaman", [8] = "Mage", [9] = "Warlock", [11] = "Druid",
}
local RACE_NAME = {
    [1] = "Human", [2] = "Orc", [3] = "Dwarf", [4] = "Night Elf", [5] = "Undead",
    [6] = "Tauren", [7] = "Gnome", [8] = "Troll", [10] = "Blood Elf", [11] = "Draenei",
}

W.CLASS_NAME, W.RACE_NAME = CLASS_NAME, RACE_NAME

function W.ClassColor(classId)
    local token = CLASS_TOKEN[classId or 0]
    local c = token and RAID_CLASS_COLORS and RAID_CLASS_COLORS[token]
    if c then return c.r, c.g, c.b end
    return 1, 1, 1
end

function W.Commify(n)
    n = tostring(math.floor(tonumber(n) or 0))
    local out = n:reverse():gsub("(%d%d%d)", "%1,"):reverse()
    return (out:gsub("^,", ""))
end

function W.Money(copper)
    copper = tonumber(copper) or 0
    return string.format("%dg %ds %dc", math.floor(copper / 10000),
                         math.floor(copper % 10000 / 100), copper % 100)
end

--- CreateFrame with a template, degrading to a bare frame if the template is absent.
--
-- Returns the frame and whether the template took. A missing template throws inside CreateFrame and
-- aborts the file, so every template this addon uses goes through here and the caller supplies its
-- own visuals when the second return is false.
function W.Create(kind, name, parent, template)
    if template then
        local ok, f = pcall(CreateFrame, kind, name, parent, template)
        if ok and f then return f, true end
    end
    return CreateFrame(kind, name, parent), false
end
