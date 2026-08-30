-- mod-bot-social: persistent social graph for playerbots.
--
-- Everything lives in the CHARACTERS database, next to the character
-- rows the guids refer to, so a character wipe and these tables stay
-- in sync.

-- ---------------------------------------------------------------
-- Who has played with whom, and who cannot stand whom.
--
-- Stored twice, once per direction, because affection is NOT
-- symmetric: A can like B more than B likes A. The two rows carry
-- different numbers on purpose.
--
-- affinity is signed. Positive is liking, negative is a grudge.
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `bot_social_bond` (
    `bot_guid`          INT UNSIGNED NOT NULL,
    `other_guid`        INT UNSIGNED NOT NULL,
    `affinity`          INT NOT NULL DEFAULT 0,

    -- how often they have met at all; drives the first-impression
    -- multiplier (early encounters count double)
    `encounters`        INT UNSIGNED NOT NULL DEFAULT 0,

    `times_grouped`     INT UNSIGNED NOT NULL DEFAULT 0,
    `minutes_together`  INT UNSIGNED NOT NULL DEFAULT 0,
    `dungeons_together` INT UNSIGNED NOT NULL DEFAULT 0,
    `battles_together`  INT UNSIGNED NOT NULL DEFAULT 0,

    -- conflict
    `grudge_events`     INT UNSIGNED NOT NULL DEFAULT 0,

    -- how much of the current grudge is only hearsay, not experience.
    -- Always <= -affinity, and it fades faster than what the bot lived
    -- through itself.
    `hearsay`           INT NOT NULL DEFAULT 0,

    `last_grudge`       VARCHAR(24) NOT NULL DEFAULT '',

    -- reciprocity: positive means bot_guid owes other_guid a favour
    `favours_owed`      INT NOT NULL DEFAULT 0,

    -- the two visible states, mirrored into WoW's own social list
    `befriended`        TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `ignored`           TINYINT UNSIGNED NOT NULL DEFAULT 0,

    `first_met`         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `last_seen`         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
                            ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`bot_guid`, `other_guid`),
    KEY `idx_affinity` (`bot_guid`, `affinity`),
    KEY `idx_last_seen` (`last_seen`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------
-- Per-bot character sheet.
--
-- archetype is how this bot spends its time; reputation is what the
-- server as a whole thinks of it, independent of any single bond.
-- A row is created the first time the bot logs in.
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `bot_social_profile` (
    `bot_guid`      INT UNSIGNED NOT NULL,
    `archetype`     VARCHAR(24) NOT NULL DEFAULT 'unset',
    `skill_tier`    TINYINT UNSIGNED NOT NULL DEFAULT 2,
    `sociability`   TINYINT UNSIGNED NOT NULL DEFAULT 50,
    `ambition`      TINYINT UNSIGNED NOT NULL DEFAULT 50,
    -- Schicht 2: Wesen, unabhaengig vom Archetyp
    `agreeableness`     TINYINT UNSIGNED NOT NULL DEFAULT 50,
    `honesty`           TINYINT UNSIGNED NOT NULL DEFAULT 50,
    `conscientiousness` TINYINT UNSIGNED NOT NULL DEFAULT 50,
    `sadism`            TINYINT UNSIGNED NOT NULL DEFAULT 0,
    -- Schicht 3: Beweggrund, abgeleitet
    `gear_motive` ENUM('means','number','display','ticket','duty')
                  NOT NULL DEFAULT 'means',
    -- Schicht 4: Verfuegbarkeit. Vorgaben = Populationsmittel,
    -- nicht das Profil eines Raiders.
    `play_minutes_week`  SMALLINT UNSIGNED NOT NULL DEFAULT 1360,
    `prime_hour`         TINYINT  UNSIGNED NOT NULL DEFAULT 18,
    `prime_span`         TINYINT  UNSIGNED NOT NULL DEFAULT 2,
    `block_minutes`      SMALLINT UNSIGNED NOT NULL DEFAULT 60,
    `commit_reliability` TINYINT  UNSIGNED NOT NULL DEFAULT 50,
    `interruptibility`   TINYINT  UNSIGNED NOT NULL DEFAULT 50,
    -- Schicht 5: Verlauf
    `stage` ENUM('entry','practice','mastery','burnout','recovery')
            NOT NULL DEFAULT 'entry',
    `stage_since` INT UNSIGNED NOT NULL DEFAULT 0,
    -- Schicht 6: aeussere Bindung, wird verdient
    `anchor_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `reputation`    INT NOT NULL DEFAULT 0,
    `created_at`    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`bot_guid`),
    KEY `idx_reputation` (`reputation`),
    KEY `idx_anchor` (`anchor_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------
-- Guild bookkeeping.
--
-- target_size is what this guild is currently trying to grow to,
-- last_recruit throttles invitations, last_schism keeps a guild from
-- falling apart twice in a row.
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `bot_social_guild` (
    `guild_id`      INT UNSIGNED NOT NULL,
    `target_size`   SMALLINT UNSIGNED NOT NULL DEFAULT 10,
    -- Normen der Gilde: dieselbe Tat wird hier anders bewertet als dort
    `loot_rule` ENUM('roll','msos','council','dkp') NOT NULL DEFAULT 'roll',
    `interrupt_tolerance` TINYINT UNSIGNED NOT NULL DEFAULT 50,
    `prime_hour` TINYINT UNSIGNED NOT NULL DEFAULT 18,
    `prime_span` TINYINT UNSIGNED NOT NULL DEFAULT 3,
    `recruited`     INT UNSIGNED NOT NULL DEFAULT 0,
    `last_recruit`  TIMESTAMP NULL DEFAULT NULL,
    `last_schism`   TIMESTAMP NULL DEFAULT NULL,
    `created_at`    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`guild_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------
-- The server's own memory of what happened.
--
-- Not used for any decision - this is the log you read when you want
-- to know why two bots hate each other, and the source the LLM
-- chatter draws on when bots gossip.
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `bot_social_event` (
    `id`          INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `actor_guid`  INT UNSIGNED NOT NULL,
    `target_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `kind`        VARCHAR(24) NOT NULL,
    `detail`      VARCHAR(128) NOT NULL DEFAULT '',
    `weight`      INT NOT NULL DEFAULT 0,
    -- Ort des Ereignisses; 0 = unbekannt (kein Player greifbar)
    `map_id`      INT UNSIGNED NOT NULL DEFAULT 0,
    `zone_id`     INT UNSIGNED NOT NULL DEFAULT 0,
    `created_at`  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_actor` (`actor_guid`, `created_at`),
    KEY `idx_kind` (`kind`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------
-- Time series for the dashboard.
--
-- Narrow (metric, value) rows so a new metric never needs a
-- migration. Written by the dashboard generator, read nowhere in
-- the module itself - it lives here so a dump of acore_characters
-- carries the history along.
-- ---------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `bot_social_snapshot` (
    `id`       INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `taken_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `metric`   VARCHAR(48) NOT NULL,
    `value`    BIGINT NOT NULL,
    PRIMARY KEY (`id`),
    KEY `idx_metric_time` (`metric`, `taken_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
