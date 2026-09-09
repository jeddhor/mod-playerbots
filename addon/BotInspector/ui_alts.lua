-- Bot Inspector -- Altbot Controls.
--
-- A full-window page listing the account's other characters, so adding alts as bots and pulling
-- them into the party is a click each instead of ".playerbots bot add <name>" followed by an invite.
--
-- Overlays both panes rather than living inside the detail pane. The roster is a view onto random
-- bots and this is a view onto one account's own characters; they share no selection and no state,
-- and interleaving them in one pane made both harder to read.

local BI = BotInspector
local W  = BI.W
local UI = BI.UI

local ROW_H       = 20
local VISIBLE     = 14
local MAX_PARTY   = 4      -- a party holds five and one of them is the player

local page = W.Panel(UI.frame, 0, 0, 0, 0.25)
page:SetPoint("TOPLEFT", 22, -44)
page:SetPoint("BOTTOMRIGHT", -18, 44)
page:Hide()
UI.altsPage = page

local title = page:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOPLEFT", 10, -8)
title:SetText("Altbot Controls")

local hint = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
hint:SetPoint("TOPLEFT", 10, -26)
hint:SetJustifyH("LEFT")
hint:SetText("Your other characters. Add logs one in as a bot and puts it in your party.")

local status = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
status:SetPoint("BOTTOMLEFT", 10, 8)
status:SetJustifyH("LEFT")
status:SetWidth(420)

-- One button per row, so the label can say what this particular alt needs.
local rows = {}

-- Rows the player has asked for but the server has not confirmed. Keyed by guid, cleared when the
-- ALTCTL reply lands or the next ALTS refresh contradicts it. Without this a click looks like it
-- did nothing for the second or two a bot login takes.
local pendingByGuid = {}

local STATE_LABEL = {
    party   = { text = "in party",  colour = { 0.4, 1.0, 0.4 } },
    bot     = { text = "bot",       colour = { 0.6, 0.8, 1.0 } },
    player  = { text = "played",    colour = { 1.0, 0.8, 0.3 } },
    offline = { text = "offline",   colour = { 0.6, 0.6, 0.6 } },
}

--- How many of the account's alts are already bots in the party.
local function partyAltCount()
    local n = 0
    for _, alt in ipairs(BI.alts) do
        if alt.state == "party" then n = n + 1 end
    end
    return n
end

--- What clicking this row should do, or nil when the row offers nothing.
-- Returns action, label, enabled.
local function actionFor(alt, full)
    if alt.state == "party" then
        return "REMOVE", "Dismiss", true
    elseif alt.state == "bot" then
        return "ADD", "Invite", not full
    elseif alt.state == "offline" then
        return "ADD", "Add", not full
    end
    -- "player" -- a human is on that character right now, including the one being played.
    return nil, "--", false
end

local function redraw()
    local list = BI.alts or {}
    local full = partyAltCount() >= MAX_PARTY

    for i = 1, VISIBLE do
        local row = rows[i]
        local alt = list[i + (page.offset or 0)]

        if not alt then
            row:Hide()
        else
            row:Show()
            -- Third argument is a pixel indent, not a flag: it positions both the icon and the text.
            W.SetRowClassIcon(row, alt.class, 4)

            local r, g, b = W.ClassColor(alt.class)
            row.text:SetText(string.format("%s  |cff999999%d %s|r", alt.name or "?", alt.level or 0,
                                           W.RACE_NAME[alt.race] or ""))
            row.text:SetTextColor(r, g, b)

            local pending = pendingByGuid[alt.guid]
            local state = STATE_LABEL[alt.state] or STATE_LABEL.offline
            if pending then
                row.right:SetText("working...")
                row.right:SetTextColor(1.0, 0.9, 0.4)
            else
                row.right:SetText(state.text)
                row.right:SetTextColor(unpack(state.colour))
            end

            local action, label, enabled = actionFor(alt, full)
            row.action = action
            row.guid   = alt.guid
            row.button:SetText(label)
            if action and enabled and not pending then
                row.button:Enable()
            else
                row.button:Disable()
            end
            -- Show/Hide rather than SetShown: SetShown does not exist in the 3.3.5 client, and
            -- nothing else in this addon uses it.
            if action then row.button:Show() else row.button:Hide() end
        end
    end

    local n = partyAltCount()
    status:SetText(string.format("%d of %d party slots used by alts.%s", n, MAX_PARTY,
                                 n >= MAX_PARTY and "  Dismiss one to add another." or ""))
end
UI.RedrawAlts = redraw

for i = 1, VISIBLE do
    local row = W.Row(page, 300, ROW_H)
    row:SetPoint("TOPLEFT", 8, -44 - (i - 1) * ROW_H)

    -- The action button is a child of the row rather than the page so it scrolls with it, and it
    -- sits outside the row's own hit area so clicking the name never fires the action by accident.
    local button = W.Create("Button", nil, page, "UIPanelButtonTemplate")
    button:SetWidth(70); button:SetHeight(18)
    button:SetPoint("LEFT", row, "RIGHT", 8, 0)
    button:SetScript("OnClick", function()
        if not row.action or not row.guid then return end

        pendingByGuid[row.guid] = true
        BI:RequestAltControl(row.action, row.guid)
        redraw()
    end)
    row.button = button

    rows[i] = row
end

W.MakeScrollable(page, function() return #(BI.alts or {}) end, function() return VISIBLE end, redraw)

BI:SetHandler("OnAlts", function()
    -- The server's view wins: anything it now reports is settled, whatever we were hoping for.
    pendingByGuid = {}
    redraw()
end)

BI:SetHandler("OnAltControl", function(guid, outcome)
    if guid then pendingByGuid[guid] = nil end

    if outcome == "pending" then
        -- The bot is still logging in. The invite is queued server-side; ask again shortly so the
        -- row settles on its own rather than waiting for the player to click Refresh.
        if guid then pendingByGuid[guid] = true end
        UI:SetStatus("logging in...")
    elseif outcome ~= "" and outcome ~= "ok" then
        UI:SetStatus("alt: %s", outcome)
    end

    redraw()
    BI:RequestAlts()
end)

--- Show or hide the page, swapping it with the two normal panes.
function UI:ShowAlts(show)
    self.altsShown = show and true or false

    if show then
        self.left:Hide(); self.right:Hide()
        page:Show()
        page.offset = 0
        UI:SetStatus("loading your characters...")
        BI:RequestAlts()
    else
        page:Hide()
        self.left:Show(); self.right:Show()
    end

    if self.altsButton then
        self.altsButton:SetText(show and "Back" or "Altbot Controls")
    end
end
