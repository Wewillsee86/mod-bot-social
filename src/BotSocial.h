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
        std::string archetype;
        uint32      skillTier    = 2;
        uint32      sociability  = 50;
        uint32      ambition     = 50;
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
