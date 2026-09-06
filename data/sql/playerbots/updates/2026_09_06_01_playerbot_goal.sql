-- What a bot is currently trying to achieve.
--
-- The NewRpg system picks its next activity from a single global weight table, so every bot on the
-- realm wants the same things in the same proportions. Goals are what make one bot a gatherer
-- saving for a mount and another a quester pushing for level 80, using the same activity set.
--
-- Only goals with a genuine target are stored. Derived ones -- "my bags are full", "I am not level
-- 80 yet" -- are recomputed each agenda tick, because persisting a fact that can be read directly
-- from the character is how the two get to disagree.
CREATE TABLE IF NOT EXISTS `playerbot_goal` (
    `guid` INT UNSIGNED NOT NULL,
    `type` TINYINT UNSIGNED NOT NULL,
    `param` INT UNSIGNED NOT NULL DEFAULT 0,
    `target` BIGINT NOT NULL DEFAULT 0,
    `progress` BIGINT NOT NULL DEFAULT 0,
    `created_at` INT UNSIGNED NOT NULL DEFAULT 0,
    `expires_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`, `type`, `param`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
