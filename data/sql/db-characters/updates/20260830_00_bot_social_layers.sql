-- mod-bot-social: die sechs Schichten
--
-- Bisher leitete der Archetyp Geselligkeit und Ehrgeiz ab - damit folgte
-- die Persoenlichkeit der Taetigkeit. Dafuer gibt es keinen empirischen
-- Beleg. Wesen, Beweggrund, Verfuegbarkeit, Verlauf und aeussere Bindung
-- sind jetzt eigene Groessen.
--
-- 'casual' war nie eine Taetigkeit, sondern ein Zeitprofil.
-- 'loner' war nie eine Taetigkeit, sondern ein Wesenszug.

ALTER TABLE `bot_social_profile`
    -- Schicht 2: Wesen. Unabhaengig vom Archetyp gewuerfelt.
    ADD COLUMN `agreeableness`     TINYINT UNSIGNED NOT NULL DEFAULT 50 AFTER `ambition`,
    ADD COLUMN `honesty`           TINYINT UNSIGNED NOT NULL DEFAULT 50 AFTER `agreeableness`,
    ADD COLUMN `conscientiousness` TINYINT UNSIGNED NOT NULL DEFAULT 50 AFTER `honesty`,
    ADD COLUMN `sadism`            TINYINT UNSIGNED NOT NULL DEFAULT 0  AFTER `conscientiousness`,

    -- Schicht 3: Beweggrund. Wird abgeleitet, nicht gewuerfelt.
    ADD COLUMN `gear_motive` ENUM('means','number','display','ticket','duty')
                             NOT NULL DEFAULT 'means' AFTER `sadism`,

    -- Schicht 4: Verfuegbarkeit. Die Vorgaben sind das Populationsmittel
    -- (Daedalus 2005: 22,7 h/Woche), NICHT das Profil eines Raiders - sonst
    -- macht ein vergessener Wuerfelwurf aus allen Feierabendraider.
    ADD COLUMN `play_minutes_week`  SMALLINT UNSIGNED NOT NULL DEFAULT 1360 AFTER `gear_motive`,
    ADD COLUMN `prime_hour`         TINYINT  UNSIGNED NOT NULL DEFAULT 18   AFTER `play_minutes_week`,
    ADD COLUMN `prime_span`         TINYINT  UNSIGNED NOT NULL DEFAULT 2    AFTER `prime_hour`,
    ADD COLUMN `block_minutes`      SMALLINT UNSIGNED NOT NULL DEFAULT 60   AFTER `prime_span`,
    ADD COLUMN `commit_reliability` TINYINT  UNSIGNED NOT NULL DEFAULT 50   AFTER `block_minutes`,
    ADD COLUMN `interruptibility`   TINYINT  UNSIGNED NOT NULL DEFAULT 50   AFTER `commit_reliability`,

    -- Schicht 5: Verlauf. stage_since ist Diagnostik, kein Zeitgeber -
    -- Uebergaenge haengen an Ereignissen, nicht am Alter der Stufe.
    ADD COLUMN `stage` ENUM('entry','practice','mastery','burnout','recovery')
                       NOT NULL DEFAULT 'entry' AFTER `interruptibility`,
    ADD COLUMN `stage_since` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `stage`,

    -- Schicht 6: aeussere Bindung. 0 = kein Anker. Wird verdient, nie
    -- gewuerfelt, und darf auf einen menschlichen Spieler zeigen.
    ADD COLUMN `anchor_guid` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `stage_since`,
    ADD KEY `idx_anchor` (`anchor_guid`);

-- Gildennormen. Dieselbe Tat wird verschieden bewertet, je nachdem, wo sie
-- geschieht: eine Sonntagsgilde verzeiht dem Vater, eine DKP-Gilde zaehlt
-- Anwesenheit und keine Gruende.
ALTER TABLE `bot_social_guild`
    ADD COLUMN `loot_rule` ENUM('roll','msos','council','dkp')
                           NOT NULL DEFAULT 'roll' AFTER `target_size`,
    ADD COLUMN `interrupt_tolerance` TINYINT UNSIGNED NOT NULL DEFAULT 50 AFTER `loot_rule`,
    ADD COLUMN `prime_hour` TINYINT UNSIGNED NOT NULL DEFAULT 18 AFTER `interrupt_tolerance`,
    ADD COLUMN `prime_span` TINYINT UNSIGNED NOT NULL DEFAULT 3  AFTER `prime_hour`;

-- ---------------------------------------------------------------------
-- Wanderung der beiden entfallenen Archetypen
--
-- Die Taetigkeit dieser Bots wurde nie erfasst - 'casual' und 'loner'
-- WAREN ihr Archetyp. Sie zu erfinden waere schlimmer, als sie neu zu
-- ziehen. Der Zusatz hinter dem Doppelpunkt sagt dem Modul, was der alte
-- Archetyp ueber den Bot wusste: es wuerfelt eine Taetigkeit und traegt
-- den geretteten Wert danach an seiner richtigen Schicht nach.
--
-- Die Werte hier NICHT vorab setzen - der Roller ueberschreibt sie beim
-- naechsten Anfassen ohnehin, und zwei Stellen mit derselben Absicht
-- laufen frueher oder spaeter auseinander.
-- ---------------------------------------------------------------------

UPDATE `bot_social_profile` SET `archetype` = 'unset:loner'
 WHERE `archetype` = 'loner';

UPDATE `bot_social_profile` SET `archetype` = 'unset:casual'
 WHERE `archetype` = 'casual';

-- ---------------------------------------------------------------------
-- Alle bestehenden Profile stammen aus der Zeit vor den Schichten und
-- haben nur die Spaltenvorgaben. stage_since = 0 ist die Marke dafuer:
-- das Modul wuerfelt sie beim naechsten Anfassen einmal nach, damit
-- nicht dreihundert Bots mit Vertraeglichkeit 50 und Fenster 18 Uhr
-- dastehen. Ruf, Spielstaerke und Bindungen bleiben unberuehrt.
--
-- Wer das NICHT will, setzt stage_since hier auf UNIX_TIMESTAMP().
-- ---------------------------------------------------------------------

UPDATE `bot_social_profile` SET `stage_since` = 0;
