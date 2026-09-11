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
local VISIBLE     = 11   -- leaves room for the self-bot panel below the list
local MAX_PARTY   = 4      -- a party holds five and one of them is the player

local page = W.Panel(UI.frame, 0, 0, 0, 0.25)
page:SetPoint("TOPLEFT", 22, -44)
page:SetPoint("BOTTOMRIGHT", -18, 44)
page:Hide()
UI.altsPage = page

local title = page:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOPLEFT", 10, -8)
title:SetText("Self & Altbot Controls")

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

-- Shown next to the state, so a glance answers "what is this bot set up to do".
local ROLE_LABEL = {
    tank = "|cff8888ff[Tank]|r",
    heal = "|cff44ff44[Heal]|r",
    dps  = "|cffff8844[DPS]|r",
    none = "|cff999999[no role]|r",
}

local STATE_LABEL = {
    party   = { text = "in party",  colour = { 0.4, 1.0, 0.4 } },
    bot     = { text = "bot",       colour = { 0.6, 0.8, 1.0 } },
    player  = { text = "played",    colour = { 1.0, 0.8, 0.3 } },
    offline = { text = "offline",   colour = { 0.6, 0.6, 0.6 } },
}

-- Forward declaration. redraw() calls this, but the button it drives cannot be built until the rows
-- exist, so the definition lands further down the file. `local function` only binds from its own
-- line onward, so without this the call inside redraw would compile to a global lookup and be nil --
-- the exact fault that stopped this addon replying at all when it was first written.
local refreshSummonParty
local redrawSelf

-- Forward-declared: the self panel calls this, and it is defined further down beside the ticker it
-- drives. `local function` binds only from its own line, so without this the calls above would
-- compile to global lookups and be nil.
local scheduleAltsRefresh

--- Whether this account may .summon at all. Reported by the server in the ZONES reply rather than
-- assumed, since the inspector's own gate and the RBAC permission are different checks.
local function canSummon()
    return BI.can and BI.can.summon
end

--- Is this alt actually in the world, and so summonable?
local function inWorld(alt)
    return alt.state == "bot" or alt.state == "party"
end

--- Drive the real .summon, exactly as the Stats page's GM controls do, rather than reimplementing a
-- teleport. The command already does the permission checks, and doing it here would mean a second
-- implementation to keep honest.
local function summon(name)
    SendChatMessage(".summon " .. name, "SAY")
end

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
-- Alliance races. Everything else is Horde.
local ALLIANCE_RACES = { [1] = true, [3] = true, [4] = true, [7] = true, [11] = true }

local function sameFactionAsPlayer(alt)
    if not alt or not alt.race then return true end
    local _, _, playerRace = UnitRace("player")
    -- UnitRace's third return is the race id on 3.3.5. If it is unavailable, do not block anything:
    -- the server refuses cross-faction grouping regardless, and a greyed-out button based on a bad
    -- guess would be worse than a refusal that explains itself.
    if not playerRace then return true end
    return (ALLIANCE_RACES[alt.race] or false) == (ALLIANCE_RACES[playerRace] or false)
end

local function actionFor(alt, full)
    -- Cross-faction grouping did not exist in 3.3.5. The panel used to offer the button anyway and
    -- the server used to honour it, which is how a blood elf ended up in a party of five humans.
    if not sameFactionAsPlayer(alt) then
        return nil, "other faction", false
    end

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
            -- Hides the buttons too, because they are children of the row. They used to be children
            -- of the page, so an empty slot kept whatever button the row last had -- a column of
            -- live buttons against blank names.
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
                -- State and role together: "in party" says whether it is here, the role says what
                -- it will actually do when a fight starts, which is the question the buttons answer.
                local roleTag = ROLE_LABEL[alt.role]
                row.right:SetText(roleTag and (state.text .. "  " .. roleTag) or state.text)
                row.right:SetTextColor(unpack(state.colour))
            end

            local action, label, enabled = actionFor(alt, full)
            row.action  = action
            row.guid    = alt.guid
            row.altName = alt.name
            row.button:SetText(label)
            if action and enabled and not pending then
                row.button:Enable()
            else
                row.button:Disable()
            end
            -- Show/Hide rather than SetShown: SetShown does not exist in the 3.3.5 client, and
            -- nothing else in this addon uses it.
            if action then row.button:Show() else row.button:Hide() end

            -- Summon is about presence, not about party membership: anything already in the world
            -- can be pulled to you, whether or not it is grouped.
            if inWorld(alt) and canSummon() then
                row.summon:Enable()
            else
                row.summon:Disable()
            end
            row.summon:Show()

            -- Role controls only mean anything for a bot that is actually running: a character a
            -- person is playing has no strategies for us to change.
            local controllable = inWorld(alt)
            for _, rb in ipairs(row.roleButtons) do
                rb:Show()
                if controllable and not pending then rb:Enable() else rb:Disable() end
            end
        end
    end

    local n = partyAltCount()
    local note = ""
    if n >= MAX_PARTY then
        note = "  Dismiss one to add another."
    elseif not canSummon() then
        note = "  Summon needs the .summon permission."
    end

    status:SetText(string.format("%d of %d party slots used by alts.%s", n, MAX_PARTY, note))
    refreshSummonParty()
    redrawSelf()
end
UI.RedrawAlts = redraw

for i = 1, VISIBLE do
    local row = W.Row(page, 230, ROW_H)
    row:SetPoint("TOPLEFT", 8, -44 - (i - 1) * ROW_H)

    -- Both buttons are children of the ROW, so hiding an empty row hides them with it. They sit
    -- outside the row's own hit area, so clicking the name never fires an action by accident.
    local button = W.Create("Button", nil, row, "UIPanelButtonTemplate")
    button:SetWidth(70); button:SetHeight(18)
    button:SetPoint("LEFT", row, "RIGHT", 8, 0)
    button:SetScript("OnClick", function()
        if not row.action or not row.guid then return end

        pendingByGuid[row.guid] = true
        BI:RequestAltControl(row.action, row.guid)
        redraw()
    end)
    row.button = button

    local summonBtn = W.Create("Button", nil, row, "UIPanelButtonTemplate")
    summonBtn:SetWidth(70); summonBtn:SetHeight(18)
    summonBtn:SetPoint("LEFT", button, "RIGHT", 4, 0)
    summonBtn:SetText("Summon")
    summonBtn:SetScript("OnClick", function()
        if row.altName then summon(row.altName) end
    end)
    row.summon = summonBtn

    -- Role buttons. Narrow and unlabelled beyond the role itself: three of them on a row leaves no
    -- space for words, and "Tank"/"Heal"/"DPS" is already the whole meaning.
    local prev = summonBtn
    row.roleButtons = {}
    for _, def in ipairs({ { "Tank", "ROLE_TANK" }, { "Heal", "ROLE_HEAL" }, { "DPS", "ROLE_DPS" } }) do
        local rb = W.Create("Button", nil, row, "UIPanelButtonTemplate")
        rb:SetWidth(46); rb:SetHeight(18)
        rb:SetPoint("LEFT", prev, "RIGHT", 3, 0)
        rb:SetText(def[1])
        rb.action = def[2]
        rb:SetScript("OnClick", function()
            if row.guid then
                pendingByGuid[row.guid] = true
                BI:RequestAltControl(rb.action, row.guid)
                redraw()
            end
        end)
        row.roleButtons[#row.roleButtons + 1] = rb
        prev = rb
    end

    rows[i] = row
end

-- Summon the whole party at once. Sits above Back and Refresh on the page rather than on the frame,
-- so it appears only where it means something.
local summonParty = W.Create("Button", nil, page, "UIPanelButtonTemplate")
summonParty:SetWidth(120); summonParty:SetHeight(24)
summonParty:SetPoint("BOTTOMRIGHT", -10, 8)
summonParty:SetText("Summon Party")
summonParty:SetScript("OnClick", function()
    -- Only what is actually in the party and in the world. Summoning something that is merely a bot
    -- somewhere else is what the per-row button is for; doing it from here would drag in alts the
    -- player did not group.
    for _, alt in ipairs(BI.alts or {}) do
        if alt.state == "party" and alt.name then
            summon(alt.name)
        end
    end
end)
UI.summonPartyButton = summonParty

--- Enable Summon Party only when there is a party to summon and the account may do it.
function refreshSummonParty()
    local n = partyAltCount()
    if n > 0 and canSummon() then
        summonParty:Enable()
    else
        summonParty:Disable()
    end
end

-- ---- self bot controls ------------------------------------------------------------------------
--
-- Sits below the alt list because it is a different kind of thing: one character, the one being
-- played, with a switch the alts do not have. Sharing the page keeps every "who is doing what"
-- control in one place rather than splitting the roster across two screens.

local selfPanel = W.Panel(page, 0, 0, 0, 0.30)
selfPanel:SetPoint("BOTTOMLEFT", 8, 38)
selfPanel:SetPoint("BOTTOMRIGHT", -8, 38)
selfPanel:SetHeight(66)
UI.selfPanel = selfPanel

local selfLabel = selfPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
selfLabel:SetPoint("TOPLEFT", 8, -6)
selfLabel:SetText("Self Bot")

local selfState = selfPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
selfState:SetPoint("TOPLEFT", 8, -22)
selfState:SetJustifyH("LEFT")

--- Toggling self-bot mode is the existing chat command, not a new protocol verb. It already handles
-- the permission checks and the repository load; duplicating that server-side would be a second
-- implementation of something that works.
local function toggleSelfBot()
    SendChatMessage(".playerbots bot self", "SAY")
end

local selfToggle = W.Create("Button", nil, selfPanel, "UIPanelButtonTemplate")
selfToggle:SetWidth(110); selfToggle:SetHeight(20)
selfToggle:SetPoint("TOPRIGHT", -8, -8)
selfToggle:SetScript("OnClick", function()
    toggleSelfBot()
    -- The command is a toggle and the reply is chat, not protocol, so ask the server what actually
    -- happened rather than assuming the click landed.
    scheduleAltsRefresh()
end)
UI.selfToggle = selfToggle

-- Role buttons for the played character. Same actions as the alts use, aimed at our own guid.
local selfRoleButtons = {}
local prevSelf = nil
for _, def in ipairs({ { "Tank", "ROLE_TANK" }, { "Heal", "ROLE_HEAL" }, { "DPS", "ROLE_DPS" } }) do
    local rb = W.Create("Button", nil, selfPanel, "UIPanelButtonTemplate")
    rb:SetWidth(46); rb:SetHeight(20)
    if prevSelf then
        rb:SetPoint("RIGHT", prevSelf, "LEFT", -3, 0)
    else
        rb:SetPoint("RIGHT", selfToggle, "LEFT", -8, 0)
    end
    rb:SetText(def[1])
    rb.action = def[2]
    rb:SetScript("OnClick", function()
        if BI.selfInfo and BI.selfInfo.guid then
            BI:RequestAltControl(rb.action, BI.selfInfo.guid)
            scheduleAltsRefresh()
        end
    end)
    selfRoleButtons[#selfRoleButtons + 1] = rb
    prevSelf = rb
end
UI.selfRoleButtons = selfRoleButtons

--- Stats display toggle.
--
-- Lives here rather than in a settings screen because it belongs to the self bot, and this is the
-- one place a person already goes to turn self-bot mode on. The display itself is a separate frame
-- that stays up while playing; see ui_selfstat.lua.
local statsCheck = W.Create("CheckButton", "BotInspectorSelfStatCheck", selfPanel, "UICheckButtonTemplate")
statsCheck:SetWidth(20); statsCheck:SetHeight(20)
statsCheck:SetPoint("TOPLEFT", 6, -38)

local statsLabel = selfPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
statsLabel:SetPoint("LEFT", statsCheck, "RIGHT", 2, 0)
statsLabel:SetText("Show stats display")

statsCheck:SetScript("OnClick", function(self)
    if UI.SetSelfStatShown then
        UI.SetSelfStatShown(self:GetChecked() and true or false)
    end
end)

statsCheck:SetScript("OnEnter", function()
    GameTooltip:SetOwner(statsCheck, "ANCHOR_RIGHT")
    GameTooltip:SetText("A small movable display of what this character's AI is doing:")
    GameTooltip:AddLine("activity, destination, stuck attempts, current target.", 1, 1, 1, true)
    GameTooltip:AddLine("Drag to move it. Click its pin to lock it in place.", 0.7, 0.7, 0.7, true)
    GameTooltip:Show()
end)
statsCheck:SetScript("OnLeave", function() GameTooltip:Hide() end)

UI.selfStatsCheck = statsCheck
UI.selfStatsLabel = statsLabel

--- Event log toggle.
--
-- A button rather than a checkbox, because unlike the stats display this is something you open to
-- read and then close again, rather than something you leave running. It still remembers whether
-- it was open across a reload, so leaving it up is a choice rather than an accident.
local logButton = W.Create("Button", "BotInspectorSelfLogButton", selfPanel, "UIPanelButtonTemplate")
logButton:SetWidth(110); logButton:SetHeight(20)
logButton:SetPoint("TOPLEFT", 6, -60)
logButton:SetText("Event log")

logButton:SetScript("OnClick", function()
    if UI.SetSelfLogShown and UI.IsSelfLogShown then
        UI.SetSelfLogShown(not UI.IsSelfLogShown())
    end
end)

logButton:SetScript("OnEnter", function()
    GameTooltip:SetOwner(logButton, "ANCHOR_RIGHT")
    GameTooltip:SetText("A scrolling record of what this character has done:")
    GameTooltip:AddLine("items sold and used, auctions listed and bought, mail read,", 1, 1, 1, true)
    GameTooltip:AddLine("gold in and out, and what killed it.", 1, 1, 1, true)
    GameTooltip:AddLine("Drag to move, drag the corner to resize, pin to lock.", 0.7, 0.7, 0.7, true)
    GameTooltip:Show()
end)
logButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

UI.selfLogButton = logButton

--- Redraw the self panel from whatever the server last reported.
function redrawSelf()
    -- The checkbox reflects saved state, not the panel being open, so it is correct on first draw
    -- and after a reload rather than only after somebody clicks it.
    if statsCheck and UI.IsSelfStatShown then
        statsCheck:SetChecked(UI.IsSelfStatShown())
    end

    local info = BI.selfInfo
    if not info then
        selfState:SetText("|cff999999not reported|r")
        selfToggle:Disable()
        for _, rb in ipairs(selfRoleButtons) do rb:Disable() end
        return
    end

    local roleTag = ROLE_LABEL[info.role]
    if info.active then
        selfState:SetText(string.format("%s  |cff44ff44ON|r%s", info.name or "?",
                                        roleTag and ("  " .. roleTag) or ""))
        selfToggle:SetText("Turn Off")
    else
        selfState:SetText(string.format("%s  |cff999999OFF|r", info.name or "?"))
        selfToggle:SetText("Turn On")
    end

    selfToggle:Enable()

    -- Roles only mean something while the bot AI is running; with self-bot off there are no
    -- strategies to change.
    for _, rb in ipairs(selfRoleButtons) do
        if info.active then rb:Enable() else rb:Disable() end
    end
end
UI.RedrawSelf = redrawSelf

W.MakeScrollable(page, function() return #(BI.alts or {}) end, function() return VISIBLE end, redraw)

-- Re-asking after an action, a few times.
--
-- Adding an alt starts a login, and a login is not instant. The first ALTS reply after the click
-- catches the character online but with no bot AI attached yet, which the server honestly reports as
-- "played" -- and because nothing asked again, the row sat there wrong until the player hit Refresh.
--
-- No C_Timer in 3.3.5, so this is an OnUpdate that hides itself once the schedule is empty.
local refreshQueue = {}
local ticker = CreateFrame("Frame")
ticker:Hide()
UI.altsRefreshTicker = ticker
ticker:SetScript("OnUpdate", function(self, elapsed)
    local remaining = false
    for i = #refreshQueue, 1, -1 do
        refreshQueue[i] = refreshQueue[i] - elapsed
        if refreshQueue[i] <= 0 then
            table.remove(refreshQueue, i)
            BI:RequestAlts()
        else
            remaining = true
        end
    end
    if not remaining then self:Hide() end
end)

--- Ask again on a short schedule, so a row settles without the player touching Refresh.
-- Spread out rather than rapid: a bot login takes a second or two, and the inspector's token bucket
-- prices every request.
function scheduleAltsRefresh()
    refreshQueue = { 1.5, 4.0, 8.0 }
    ticker:Show()
end

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
    scheduleAltsRefresh()
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
        self.altsButton:SetText(show and "Back" or "Self & Altbot Controls")
    end
end
