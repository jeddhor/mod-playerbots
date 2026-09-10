-- Take ITEM_FLAG_MULTI_DROP back off tradeable commodities (corrects 2026_09_07_00).
--
-- That change gave collect-X quest drops per-player looting, which is right for quest items and
-- wrong for anything with a market. Its filter was "quest-required, count above one, drops from a
-- creature", and a great many ordinary crafting reagents satisfy all three -- cloth is required by
-- quests, so every cloth tier was flagged, and with it leather, herbs, ore, elemental essences,
-- gems and potions.
--
-- The effect on a realm full of grouped bots is not subtle. Free-for-all loot gives every party
-- member their own copy of the same drop, so the realm's supply of these goods is multiplied by
-- party size on every kill. Observed as Linen Cloth appearing to duplicate: it was not duplicating,
-- it was flagged for exactly that behaviour. The worst of them are not cloth -- Primordial Saronite
-- sells for 70000 and Dark Iron Ore for 100000.
--
-- Reverted here: item classes 0 (Consumable), 3 (Gem) and 7 (Trade Goods) -- 105 items. These are
-- crafted with, socketed, drunk or sold, so multiplying them distorts the economy.
--
-- Kept: classes 12 (Quest), 13 (Key) and 15 (Miscellaneous). Those exist only to be handed in, have
-- no secondary market, and are the category the original change was actually about.
--
-- Only the flag this module set is cleared. Items that carried it in stock AzerothCore data are not
-- touched, because the id list below was taken from 2026_09_07_00's own statements.
--
-- Reversal: UPDATE `item_template` SET `Flags` = `Flags` | 0x800 WHERE `entry` IN (...same ids...);

UPDATE `item_template` SET `Flags` = `Flags` & ~0x800 WHERE `entry` IN (
  723, 729, 730, 731, 769, 814, 1015, 1080, 1081, 1468, 2251, 2296, 2318, 2319, 2447, 2449,
  2455, 2589, 2592, 2633, 2886, 2924, 3164, 3172, 3173, 3174, 3356, 3357, 3404, 3466, 3712,
  3820, 3857, 3864, 4234, 4265, 4304, 4306, 4338, 4363, 4375, 4479, 4480, 4481, 4595, 4611,
  4625, 5465, 5469, 5635, 6464, 6486, 6887, 7075, 7077, 7078, 7079, 7080, 7081, 7127, 7910,
  7974, 8150, 8152, 8153, 8165, 8173, 8831, 8836, 8838, 8846, 8932, 9061, 11325, 11370, 11405,
  12207, 12361, 12800, 12804, 13444, 13446, 14047, 14227, 15416, 15564, 15994, 18562, 20520,
  21024, 21884, 22574, 22644, 22786, 22790, 22829, 22832, 23676, 29443, 31670, 31671, 33470,
  39681, 43013, 49908
);
