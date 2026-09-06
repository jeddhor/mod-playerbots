-- Observed market value per item, maintained by BotEconomyMgr.
--
-- The index is seeded from item_template (vendor price scaled by quality and class) and then
-- drifts toward what the auction house actually bears, via an exponential moving average of the
-- lowest per-unit buyout observed on each sampling pass. Persisting it means a restart does not
-- throw the realm's price discovery away and start every item back at its vendor-derived guess.
--
-- `observations` is kept so a price backed by one sighting can be told apart from one backed by
-- hundreds; `depth` is the listing count at the last sample and drives posting suppression.
CREATE TABLE IF NOT EXISTS `playerbot_market_price` (
    `item_id` INT UNSIGNED NOT NULL,
    `price` INT UNSIGNED NOT NULL DEFAULT 0,
    `depth` INT UNSIGNED NOT NULL DEFAULT 0,
    `observations` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`item_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
