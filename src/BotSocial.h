/*
 * mod-bot-social - a persistent social graph for playerbots.
 *
 * The module observes the world through core script hooks only. It
 * never links against mod-playerbots, so it keeps working when that
 * module is updated. Bots are recognised through
 * WorldSession::IsBot().
 *
 * The model deliberately mirrors a few things about how people
 * actually behave:
 *   - affection is one-sided (two rows per pair, different numbers)
 *   - grudges exist, and they fade slower than friendships
 *   - the first few encounters weigh double
 *   - nobody keeps track of eight hundred people (Dunbar cap)
 */

#ifndef MOD_BOT_SOCIAL_H
#define MOD_BOT_SOCIAL_H

#include "Define.h"
#include "ObjectGuid.h"

#include <ctime>
#include <string>
#include <vector>

class Group;
class Guild;
class Item;
class Player;

namespace BotSocial
{
    // ---- configuration -------------------------------------------------

    struct Config
    {
        bool  enable            = false;
        bool  debug             = false;

        // ---- positive events ----
        int32 gainGroupJoin     = 2;
        int32 gainPerMinute     = 1;
        int32 gainDungeon       = 15;
        int32 gainBattleground  = 8;
        int32 gainResurrect     = 12;   // for the one being raised
        int32 gainResurrector   = 4;    // for the one doing the raising
        int32 maxMinutesPerStint = 60;

        // ---- conflict ----
        bool  conflictEnable    = true;
        int32 lossKicked        = 40;   // thrown out of a group
        int32 lossKickedLfg     = 55;   // thrown out by a vote
        int32 lossBailed        = 25;   // left the group inside an instance
        int32 lossNinjaLoot     = 35;   // rolled need against another need
        int32 lossGuildKick     = 60;   // thrown out of the guild
        int32 lossBankDrain     = 8;    // emptied the guild bank
        uint32 bankDrainCopper  = 500000; // withdrawal that counts as draining
        int32 lossGank          = 20;   // killed someone far below their level
        uint32 gankLevelGap     = 8;
        int32 lossDuel          = 2;    // a sting, not a grudge

        // ---- human quirks ----
        uint32 firstImpressions = 3;    // encounters that count double
        int32  firstImpressionMul = 2;
        int32  clashPenalty     = 2;    // per grouping between clashing types

        // ---- decay: bad memories are stickier than good ones ----
        uint32 decayIntervalHours   = 24;
        int32  decayPositive        = 1;
        int32  decayNegative        = 1;
        uint32 decayNegativeDivisor = 3; // grudges decay 1/3 as often

        // ---- Dunbar ----
        uint32 maxBondsPerBot   = 50;

        // ---- visible consequences ----
        bool   friendsEnable      = true;
        int32  friendsThreshold   = 60;
        bool   avoidEnable        = true;
        int32  avoidThreshold     = -30;  // refuse to group below this
        bool   ignoreEnable       = true;
        int32  ignoreThreshold    = -90;  // put on the real ignore list

        // ---- the player is a social actor too, with guard rails ----
        bool   playerEnable       = true;
        bool   playerCanBeIgnored = false;
        uint32 playerGrudgeSpeedup = 2;   // grudges against you fade faster

        // ---- guild recruitment ----
        bool   recruitEnable      = true;
        uint32 recruitInterval    = 300;
        uint32 recruitPerTick     = 2;
        uint32 recruitCooldown    = 900;
        uint32 guildTargetMin     = 12;
        uint32 guildTargetMax     = 45;
        // Recruiting past the size mod-playerbots considers "full" is wasted work: it stops
        // assigning bots to such a guild. 0 = read AiPlayerbot.RandomBotGuildSizeMax, which is
        // the value that actually decides it. A positive number overrides that.
        uint32 guildCapacityCap   = 0;
        int32  recruitMinAffinity = 25;
        int32  recruitMinReputation = -50;
        bool   recruitStrangers   = true;

        // ---- rank progression ----
        bool   ranksEnable         = true;
        int32  rankPromoteAffinity = 150;
        uint32 rankPromoteInterval = 3600;

        // ---- guild schism ----
        bool   schismEnable        = false;
        uint32 schismInterval      = 3600;
        uint32 schismServerCooldown = 604800; // once a week, server wide
        uint32 schismGuildCooldown = 2592000; // a guild splits once a month
        int32  schismLeaderDislike = -40;     // ringleader vs guild master
        int32  schismCliqueBond    = 120;     // sum inside the clique
        uint32 schismMinFollowers  = 2;
        uint32 schismMaxFollowers  = 6;
        uint32 schismMinGuildSize  = 12;

        // ---- reputation ----
        bool   reputationEnable   = true;
        int32  reputationPerGrudge = 3;
        int32  reputationPerFavour = 1;

        // ---- Geselligkeit: nicht jeder will in eine Gruppe ----
        //
        // Jeder Bot wuerfelt beim ersten Kontakt eine Geselligkeit von 0
        // bis 100; ein Einzelgaenger liegt bei 0 bis 20. Bisher stand die
        // Zahl nur in der Datenbank. Hier entscheidet sie mit, ob jemand
        // ueberhaupt zusammen spielen will - als Neigung, nicht als Mauer.
        bool   sociabilityEnable   = false;
        int32  sociabilityInviteMalus   = 10; // selbst fragen kostet Ueberwindung
        int32  sociabilityPerMemberMalus = 3; // je voller die Gruppe, desto eher nein
        int32  sociabilityAffinityDivisor = 4; // mit Freunden geht auch ein Muffel mit
        uint32 sociabilityMoodHours = 6;  // so lange bleibt die Laune dieselbe
        int32  sociabilityFloor    = 5;   // auch ein Eremit sagt manchmal ja

        // ---- Geruechte: Groll verbreitet sich ueber Freunde ----
        //
        // Bisher war der Ruf koerperlos: der Server wusste sofort, wer
        // sich danebenbenommen hat. Hier erzaehlt stattdessen das Opfer
        // seinen Freunden davon, und wie viel haengenbleibt, entscheidet
        // deren Vertrauen zum Erzaehler - nicht die Wahrheit.
        bool   gossipEnable       = false;
        int32  gossipMinSeverity  = 25;  // kleinere Vorfaelle spricht niemand an
        int32  gossipMinTrust     = 40;  // ab dieser Bindung hoert jemand zu
        int32  gossipFullTrust    = 120; // ab hier glaubt er es ungefiltert
        int32  gossipPercent      = 25;  // Anteil des Vorfalls beim Zuhoerer
        uint32 gossipMaxListeners = 5;   // pro Vorfall
        uint32 gossipMaxStrangers = 2;   // neue Bindungen pro Vorfall
        uint32 gossipDecayFactor  = 3;   // Hoerensagen verblasst schneller
        bool   gossipAboutPlayers = false; // ueber dich wird nicht getratscht

        // ---- trace: every award becomes a bot_social_event row ----
        bool   traceEnable        = false;
        int32  traceMinWeight     = 0;   // |wirksame Vergabe| darunter: still
        uint32 traceRetentionDays = 14;  // ältere Zeilen beim Start löschen

        // ---- housekeeping ----
        uint32 flushInterval      = 60;

        // GM-Aktionen (aktives .gm on) erzeugen keine Ereignisse und
        // keine Punkte - Testen soll die Werte nicht verfaelschen.
        bool   ignoreGameMasters  = true;

        // how to recognise a bot-led guild without linking playerbots
        std::string authDatabase     = "acore_auth";
        std::string botAccountPrefix = "RNDBOT";

        // ---- Schichten (Fassung 2) --------------------------------------
        // Wesen wird jetzt unabhaengig vom Archetyp gewuerfelt. Vorher leitete
        // ArchetypeDef Geselligkeit und Ehrgeiz aus der Taetigkeit ab - die
        // Forschung stuetzt diesen Schluss nicht.
        bool   layersEnable        = true;

        // Schicht 2: Wesen. Normalverteilt, hart auf 1..99.
        int32  traitMean           = 50;
        int32  traitSpread         = 18;

        // Sadismus ist nicht normalverteilt: die Masse liegt nahe null,
        // ein schmaler Rand liegt hoch. Volkmer 2023 zeigt, dass Sadismus
        // nur die oberen Quantile erklaert, nicht das niedrige Niveau.
        uint32 sadismTailPercent   = 5;  // so viele landen im Rand
        uint32 sadismQuietMax      = 15;  // Obergrenze des ruhigen Topfes
        uint32 sadismTailLo        = 40;  // Schwelle, die 5 % erreichen

        // Schicht 4: Zeitprofile. Die fuenf Bestandteile werden GEMEINSAM
        // gezogen, nicht einzeln - sonst entstehen unmoegliche Menschen.
        uint32 timeWeightEvening   = 40;  // Feierabend, langer Block
        uint32 timeWeightAlongside = 35;  // nebenher, kurz, unterbrechbar
        uint32 timeWeightHeavy     = 13;  // viel Zeit, breites Fenster
        uint32 timeWeightNight     = 12;  // nach 22 Uhr, ueber Mitternacht
        uint32 playMinutesMean     = 1360; // 22,7 h/Woche (Daedalus 2005)
        uint32 playMinutesSpread   = 846;  // SD 14,1 h
        uint32 playMinutesMin      = 180;
        uint32 playMinutesMax      = 4200;

        // Wer so kurz kann und so oft weg muss, betritt keine Instanz.
        uint32 instanceMinBlock     = 40;
        uint32 instanceMaxInterrupt = 80;

        // Schicht 6: Anker werden verdient, nie gewuerfelt.
        bool   anchorEnable        = true;
        uint32 anchorMinDungeons   = 4;
        uint32 anchorMinMinutes    = 240;
        int32  anchorBreakAt       = 40;  // ab diesem Schlag reisst er

        // ---- Gilden entstehen aus Cliquen (Schritt 3c) ------------------
        // mod-playerbots setzt Bots beim Serverstart wahllos in Gilden
        // (AiPlayerbot.RandomBotGuildCount). Das ist genau das, was dieses
        // Modul nicht will: eine Gilde soll entstehen, WEIL sich Leute
        // kennen. RandomBotGuildCount gehoert deshalb auf 0.
        bool   foundEnable         = true;
        uint32 foundInterval       = 900;    // so oft wird geschaut
        uint32 foundServerCooldown = 3600;   // hoechstens eine Gruendung je
        uint32 foundMaxGuilds      = 60;     // Obergrenze fuer Botgilden
        uint32 foundMinFriends     = 4;      // so viele muessen mitkommen
        int32  foundMinBond        = 80;     // 0 = Friends.Threshold nehmen
        uint32 foundMinAmbition    = 60;     // wer gruendet, will etwas
        uint32 foundMinSociability = 45;
        uint32 foundMinLevel       = 10;
        // Namenstil. Auf deutschen Realms sind englische Gildennamen
        // ueblich und waren es immer; Humor ist eine kraeftige Minderheit.
        uint32 nameEnglishPercent  = 40;
        uint32 nameHumorPercent    = 15;

        // ---- Zeitfenster als Gildenidentitaet (Schritt 3b) --------------
        // Feierabendgilden, Sonntagsgilden, Nachtschichtgilden: eine Gilde
        // ist auch ein Zeitfenster. Wer nicht hineinpasst, wird seltener
        // angeworben und spaltet leichter ab.
        bool   guildWindowEnable   = true;
        int32  recruitWindowBonus  = 8;   // je Stunde Ueberschneidung
        uint32 recruitMinSociability = 20; // darunter tritt niemand bei
        uint32 schismWindowGapMax  = 1;   // so wenig Ueberschneidung = fremd
        int32  schismTimeBonus     = 40;  // Vorsprung fuer den Abweichler

        // ---- Der Abbruch bekommt eine Ursache (Schritt 3) ---------------
        // Wer die Instanz verlaesst, weil seine Sitzung endet, ist etwas
        // anderes als wer einfach geht. Bisher sah beides gleich aus - in
        // der Zuneigungszahl wie im Protokoll.
        bool   bailReasonEnable    = true;
        uint32 bailExcuseFrom      = 70;  // ab dieser Unterbrechbarkeit
        int32  bailExcusePercent   = 40;  // so viel Groll bleibt
        uint32 bailAggravateBelow  = 30;  // unter dieser Gewissenhaftigkeit
        int32  bailAggravatePercent = 150; // so viel Groll wird daraus

        // ---- Schicht 2 wirkt (Schritt 2) --------------------------------
        // Vertraeglichkeit entscheidet, wie schwer jemand etwas nimmt und
        // wie lange er es nachtraegt. Ehrlichkeit und Gewissenhaftigkeit
        // bleiben absichtlich noch stumm: die eine braucht den Beutewurf
        // (Schritt 5), die andere Zusagen (Schritt 3). Werte zu benutzen,
        // bevor es das Ereignis dazu gibt, waere erfundene Kausalitaet.
        bool   traitsAffect        = true;
        int32  traitGrudgeSwing    = 80;  // Ausschlag auf die Grollhoehe
        int32  traitAvoidSwing     = 40;  // Ausschlag auf die Meideschwelle
        uint32 traitForgiveFrom    = 70;  // ab hier verblasst Groll jede Runde
        uint32 traitGrudgeHoldBelow = 30; // darunter halb so oft

        // Hoechstzahl Anweisungen je Schreibrunde. Ohne das laege beim
        // ersten Start nach der Wanderung die gesamte Bevoelkerung in
        // einer einzigen Transaktion - bei mehreren tausend Bots und
        // zwanzigspaltigen Profilzeilen haelt das die Charakterdatenbank
        // unnoetig lange fest. Der Rest bleibt schmutzig und geht in der
        // naechsten Runde raus; '.social stats' zeigt den Rueckstand.
        uint32 flushMaxStatements  = 2000;
    };

    Config const& Cfg();
    void LoadConfig();

    // ---- identity ------------------------------------------------------

    bool IsBot(Player* player);

    // ---- positive events ------------------------------------------------

    void OnGroupMemberAdded(Group* group, ObjectGuid guid);
    // `left` distinguishes someone walking out on their own from a
    // group that simply dissolved. Only the former is a betrayal.
    void OnGroupMemberRemoved(Group* group, ObjectGuid guid,
                              bool kicked, bool lfgKick, bool left,
                              ObjectGuid kicker);
    void OnGroupDisbanded(Group* group);
    void OnDungeonEntered(Player* player, uint32 mapId);
    void OnBattlegroundEntered(Player* player);
    void OnResurrected(Player* player);
    // Nur Protokoll (kind='level_up'), keine Punkte - fuer die
    // Level-Kurven des Dashboards.
    void OnLevelUp(Player* player, uint8 oldLevel);

    // ---- conflict --------------------------------------------------------

    void OnLootRoll(Player* player, Item* item, uint32 voteType);
    void OnGuildMemberRemoved(Guild* guild, Player* player, bool kicked);
    void OnGuildMoneyWithdrawn(Guild* guild, Player* player, uint32 amount);
    void OnPvpKill(Player* killer, Player* killed);
    void OnDuelFinished(Player* winner, Player* loser);

    // ---- decisions the world asks us about --------------------------------

    // Would this player refuse to join that group?
    bool WouldRefuseGroup(Player* player, Group* group);
    // Would this player refuse to invite that character?
    bool WouldRefuseInvite(Player* player, std::string const& targetName);

    // ---- lifecycle -------------------------------------------------------

    void OnLogin(Player* player);
    void Update(uint32 diff);
    void Startup();
    void Shutdown();

    // ---- queries used by the command ------------------------------------

    struct BondRow
    {
        uint32      otherGuid;
        std::string otherName;
        int32       affinity;
        uint32      timesGrouped;
        uint32      minutesTogether;
        uint32      dungeons;
        uint32      grudges;
        std::string lastGrudge;
        bool        befriended;
        bool        ignored;
    };

    struct ProfileRow
    {
        // Schicht 1: Taetigkeit
        std::string archetype;
        // Koennen - eigene Groesse, weder Wesen noch Taetigkeit
        uint32      skillTier    = 2;
        // Schicht 2: Wesen
        uint32      sociability  = 50;
        uint32      ambition     = 50;
        uint32      agreeableness     = 50;
        uint32      honesty           = 50;
        uint32      conscientiousness = 50;
        uint32      sadism            = 0;
        // Schicht 3: Beweggrund (abgeleitet, nicht gewuerfelt)
        std::string gearMotive   = "means";
        // Schicht 4: Verfuegbarkeit
        uint32      playMinutesWeek   = 1360;
        uint32      primeHour         = 18;
        uint32      primeSpan         = 2;
        uint32      blockMinutes      = 60;
        uint32      commitReliability = 50;
        uint32      interruptibility  = 50;
        // Schicht 5: Verlauf
        std::string stage        = "entry";
        // Schicht 6: Anker (0 = keiner)
        uint32      anchorGuid   = 0;
        std::string anchorName;

        int32       reputation   = 0;
        bool        found        = false;
    };

    std::vector<BondRow> TopBonds(uint32 botGuid, uint32 limit);
    std::vector<BondRow> WorstBonds(uint32 botGuid, uint32 limit);
    ProfileRow GetProfile(uint32 botGuid);
    bool ResolveCharacter(std::string const& name, uint32& guidOut);

    struct Stats
    {
        uint64 bondRows      = 0;
        uint64 friendships   = 0;
        uint64 grudges       = 0;
        uint64 ignores       = 0;
        uint64 guildsTracked = 0;
        uint64 recruited     = 0;
        uint64 schisms       = 0;
        uint64 pendingWrites = 0;
    };

    Stats Snapshot();
}

#endif // MOD_BOT_SOCIAL_H
