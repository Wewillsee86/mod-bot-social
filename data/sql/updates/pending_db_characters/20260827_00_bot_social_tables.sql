-- mod-bot-social: same tables as the base file.
--
-- Duplicated here on purpose. AzerothCore applies files under base/
-- when a database is set up from scratch, but an already-populated
-- characters database only ever gets files from updates/. Every
-- statement is CREATE TABLE IF NOT EXISTS, so running both is
-- harmless - and this way the tables appear no matter which path the
-- updater takes.

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
    `reputation`    INT NOT NULL DEFAULT 0,
    `created_at`    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`bot_guid`),
    KEY `idx_reputation` (`reputation`)
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
    `created_at`  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_actor` (`actor_guid`, `created_at`),
    KEY `idx_kind` (`kind`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
