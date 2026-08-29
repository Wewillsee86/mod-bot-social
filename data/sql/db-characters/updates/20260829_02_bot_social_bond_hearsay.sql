-- Geruechte: wie viel des aktuellen Grolls nur gehoert und nicht
-- erlebt ist. Der Wert ist immer <= -affinity und verblasst schneller
-- (BotSocial.Gossip.DecayFactor) - was man selbst erlebt hat, klebt,
-- was einem erzaehlt wurde, ist in ein paar Wochen vergessen.
ALTER TABLE `bot_social_bond`
    ADD COLUMN `hearsay` INT NOT NULL DEFAULT 0 AFTER `grudge_events`;
