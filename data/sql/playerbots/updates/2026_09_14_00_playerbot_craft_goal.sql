-- P10.5 -- the recipe a bot is currently farming materials for.
--
-- Persisted rather than held in memory because it is a multi-trip errand, not an activity. A bot
-- that decides to make a Greater Magic Wand and needs four more Lesser Magic Essences will cross a
-- zone, fight for a while, log out, and come back; a goal that did not survive that would restart
-- from nothing every time and the bot would never finish anything.
CREATE TABLE IF NOT EXISTS `playerbot_craft_goal` (
    `guid` INT UNSIGNED NOT NULL,
    `spell` INT UNSIGNED NOT NULL,          -- the recipe
    `item` INT UNSIGNED NOT NULL,           -- what it makes, kept so the goal reads without a DBC lookup
    `created_at` INT UNSIGNED NOT NULL DEFAULT 0,
    `expires_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)                    -- one goal at a time: a bot chasing two recipes finishes neither
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Recipes that keep being chosen and never completed.
--
-- The same lesson as the quest blacklist, which exists because bots otherwise rediscover the same
-- dead end forever. A reagent that looks farmable in the index but is not in practice -- the spawns
-- are contested, or behind a wall, or the drop rate is a rounding error -- will strand every bot
-- that tries it, one after another, unless the failures are remembered realm-wide.
CREATE TABLE IF NOT EXISTS `playerbot_craft_goal_failure` (
    `spell` INT UNSIGNED NOT NULL,
    `failures` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
