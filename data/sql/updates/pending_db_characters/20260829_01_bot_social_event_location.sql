-- Ort am Ereignis (Idee aus mod-player-statistics): map_id/zone_id
-- machen Auswertungen wie "wo entstehen Grolle?" moeglich.
-- 0 = Ort unbekannt (Ereignisse ohne greifbaren Player, z.B. der
-- Minuten-Tick oder Anwerbungen aus dem Weltlauf).
ALTER TABLE `bot_social_event`
    ADD COLUMN `map_id`  INT UNSIGNED NOT NULL DEFAULT 0 AFTER `weight`,
    ADD COLUMN `zone_id` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `map_id`;
