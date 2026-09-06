-- How many times a bot has listed a particular item without it selling.
--
-- Auctions run at most 48 hours, so an item that has been through several full listings is one the
-- market has actually declined, not one that merely needs more time. Past a threshold the bot cuts
-- its losses and vendors the item, which is what stops the auction house silently accumulating a
-- permanent floor of goods nobody will ever bid on.
--
-- Keyed by item GUID because that survives the round trip: posting, expiring, being mailed back and
-- being relisted all keep the same item_instance.
CREATE TABLE IF NOT EXISTS `playerbot_auction_attempts` (
    `item_guid` INT UNSIGNED NOT NULL,
    `attempts` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`item_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
