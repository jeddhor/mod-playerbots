-- Realm-wide record of which quests the NewRpg system has repeatedly failed to make progress on.
--
-- This replaces PlayerbotAI::lowPriorityQuest, which was a per-bot in-memory set: every bot
-- independently spent 5 minutes discovering the same unworkable quest, and the knowledge was
-- thrown away on logout. `success_count` exists so a single bot's bad pathing run can never
-- blacklist a quest that other bots have demonstrably completed.
CREATE TABLE IF NOT EXISTS `playerbot_quest_blacklist` (
    `quest_id` INT UNSIGNED NOT NULL,
    `fail_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `success_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
