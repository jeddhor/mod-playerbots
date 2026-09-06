-- A bot's persistent disposition: what kind of player it behaves like.
--
-- Goals (playerbot_goal) say what a bot wants right now and change constantly. An archetype is
-- fixed for the life of the character and shifts its baseline: a Gatherer and a Socialite with
-- identical goals still spend their days differently. Without it, goals alone converge every bot on
-- the same behaviour, because every bot eventually wants the same things.
--
-- Assigned once, on first evaluation, and never re-rolled -- a bot whose personality changed every
-- login would undo the point of having one.
CREATE TABLE IF NOT EXISTS `playerbot_profile` (
    `guid` INT UNSIGNED NOT NULL,
    `archetype` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `assigned_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
