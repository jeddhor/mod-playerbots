-- Bot Inspector -- suppress loot roll dialogs while self-bot mode is on.
--
-- When a player runs their own character as a self bot, the bot answers loot rolls server-side
-- (BotRollMgr casts the vote after a short delay). The client still builds the roll frame, because
-- as far as it knows nobody has answered -- and once the server has recorded the bot's vote it
-- discards the player's click as a duplicate, so nothing ever arrives to close the frame.
--
-- That is why the dialog needed clicking twice and still lingered: the first click was answering a
-- roll that had already been answered.
--
-- Hiding the frame is the honest fix on this side. The roll genuinely is being handled; the dialog
-- is asking a question that has already been decided, and a stale prompt is worse than no prompt.
-- Only while self-bot mode is ON: with it off the player is playing normally and the dialog is
-- theirs to answer.

local BI = BotInspector

-- FrameXML defines four; fall back in case a UI replacement changes it.
local function frameCount()
    return NUM_GROUP_LOOT_FRAMES or 4
end

local function selfBotActive()
    return BI.selfInfo ~= nil and BI.selfInfo.active == true
end

--- Hide any roll frame that is currently showing.
-- Clearing rollID and rollTime as well as hiding, because GroupLootFrame_OpenNewFrame picks the
-- first frame that is not shown -- leaving stale ids behind would work, but a frame that still
-- believes it is mid-roll is the kind of thing that surprises somebody later.
local function hideRollFrames()
    for i = 1, frameCount() do
        local frame = getglobal and getglobal("GroupLootFrame" .. i) or _G["GroupLootFrame" .. i]
        if frame and frame:IsShown() then
            frame.rollID = nil
            frame.rollTime = nil
            frame:Hide()
        end
    end
end

-- Driven by the event and by a short poll.
--
-- The event alone is not enough: START_LOOT_ROLL reaches every registered handler, and FrameXML's
-- own handler may build the frame after ours has run, leaving it on screen. A brief poll afterwards
-- catches it whichever order they fire in, then stops. Polling forever would be wasteful for
-- something that only matters for a second or two after a roll begins.
local SWEEP_SECONDS = 2.0
local sweepLeft = 0

local watcher = CreateFrame("Frame")
watcher:RegisterEvent("START_LOOT_ROLL")
watcher:RegisterEvent("PLAYER_ENTERING_WORLD")
BI.lootRollWatcher = watcher

watcher:SetScript("OnEvent", function(self, event)
    if event == "PLAYER_ENTERING_WORLD" then
        -- Learn whether self-bot mode is on without the player opening the panel. One request at
        -- login, not a poll: the answer only changes when they toggle it, and toggling goes through
        -- our own panel, which refreshes anyway.
        if BI.RequestAlts then BI:RequestAlts() end
        return
    end

    if not selfBotActive() then
        -- Unknown state is not the same as "off". Ask, so the next roll is handled correctly rather
        -- than silently leaving stale dialogs for the rest of the session.
        if BI.selfInfo == nil and BI.RequestAlts then BI:RequestAlts() end
        return
    end

    hideRollFrames()
    sweepLeft = SWEEP_SECONDS
    self:Show()
end)

watcher:SetScript("OnUpdate", function(self, elapsed)
    sweepLeft = sweepLeft - elapsed

    if selfBotActive() then
        hideRollFrames()
    end

    -- Stop as soon as the budget is spent rather than on the following tick: a watcher that keeps
    -- running for one more frame than it needs to is a small thing, but "it stops when it says it
    -- stops" is the property worth having.
    if sweepLeft <= 0 then
        self:Hide()
    end
end)

watcher:Hide()
