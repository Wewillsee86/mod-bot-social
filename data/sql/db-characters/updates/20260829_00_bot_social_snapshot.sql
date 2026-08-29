-- mod-bot-social: Zeitreihen-Tabelle fuer das Dashboard.
--
-- Schmal und lang statt breit: (metric, value) pro Zeile, damit eine
-- neue Kennzahl nie eine Migration braucht. Geschrieben wird sie vom
-- Dashboard-Generator (F:\Azerothcore\dashboard\build_dashboard.py),
-- nicht vom Modul selbst - sie liegt hier, damit sie beim Sichern von
-- acore_characters mitwandert.
CREATE TABLE IF NOT EXISTS `bot_social_snapshot` (
    `id`       INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `taken_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `metric`   VARCHAR(48) NOT NULL,
    `value`    BIGINT NOT NULL,
    PRIMARY KEY (`id`),
    KEY `idx_metric_time` (`metric`, `taken_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
