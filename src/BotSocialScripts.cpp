/*
 * mod-bot-social - script registrations.
 *
 * Every observation point is a plain core hook. mod-playerbots
 * registers neither a GroupScript nor a GuildScript, so nothing here
 * collides with it and nothing here needs it to be present.
 */

#include "BotSocial.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <sstream>
#include <string>

using namespace Acore::ChatCommands;

// -----------------------------------------------------------------------
// World: configuration, startup, the periodic tick
// -----------------------------------------------------------------------

class BotSocialWorldScript : public WorldScript
{
public:
    BotSocialWorldScript()
        : WorldScript("BotSocialWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                       WORLDHOOK_ON_STARTUP,
                       WORLDHOOK_ON_UPDATE,
                       WORLDHOOK_ON_SHUTDOWN}) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        BotSocial::LoadConfig();
    }

    void OnStartup() override
    {
        BotSocial::Startup();
    }

    void OnUpdate(uint32 diff) override
    {
        BotSocial::Update(diff);
    }

    // Without this, up to a full flush interval of affinity changes is
    // lost on every restart.
    void OnShutdown() override
    {
        BotSocial::Shutdown();
    }
};

// -----------------------------------------------------------------------
// Groups: where bonds are born and where most grudges start
// -----------------------------------------------------------------------

class BotSocialGroupScript : public GroupScript
{
public:
    BotSocialGroupScript()
        : GroupScript("BotSocialGroupScript",
                      {GROUPHOOK_ON_ADD_MEMBER,
                       GROUPHOOK_ON_REMOVE_MEMBER,
                       GROUPHOOK_ON_DISBAND}) {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        BotSocial::OnGroupMemberAdded(group, guid);
    }

    void OnRemoveMember(Group* group, ObjectGuid guid,
                        RemoveMethod method, ObjectGuid kicker,
                        char const* /*reason*/) override
    {
        bool kicked  = (method == GROUP_REMOVEMETHOD_KICK)
                    || (method == GROUP_REMOVEMETHOD_KICK_LFG);
        bool lfgKick = (method == GROUP_REMOVEMETHOD_KICK_LFG);
        bool left    = (method == GROUP_REMOVEMETHOD_LEAVE);

        BotSocial::OnGroupMemberRemoved(group, guid, kicked, lfgKick,
                                        left, kicker);
    }

    void OnDisband(Group* group) override
    {
        BotSocial::OnGroupDisbanded(group);
    }
};

// -----------------------------------------------------------------------
// Guilds
// -----------------------------------------------------------------------

class BotSocialGuildScript : public GuildScript
{
public:
    BotSocialGuildScript()
        : GuildScript("BotSocialGuildScript",
                      {GUILDHOOK_ON_CREATE,
                       GUILDHOOK_ON_DISBAND,
                       GUILDHOOK_ON_REMOVE_MEMBER,
                       GUILDHOOK_ON_MEMBER_WITDRAW_MONEY}) {}

    void OnCreate(Guild* guild, Player* leader,
                  std::string const& /*name*/) override
    {
        if (!BotSocial::Cfg().enable || !guild || !leader)
            return;

        if (!BotSocial::IsBot(leader))
            return;

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO bot_social_guild (guild_id, target_size) "
            "VALUES ({}, {})",
            guild->GetId(), BotSocial::Cfg().guildTargetMin);
    }

    void OnDisband(Guild* guild) override
    {
        if (!guild)
            return;

        CharacterDatabase.Execute(
            "DELETE FROM bot_social_guild WHERE guild_id = {}",
            guild->GetId());
    }

    void OnRemoveMember(Guild* guild, Player* player,
                        bool isDisbanding, bool isKicked) override
    {
        if (isDisbanding)
            return;

        BotSocial::OnGuildMemberRemoved(guild, player, isKicked);
    }

    void OnMemberWitdrawMoney(Guild* guild, Player* player,
                              uint32& amount, bool isRepair) override
    {
        if (isRepair)
            return;

        BotSocial::OnGuildMoneyWithdrawn(guild, player, amount);
    }
};

// -----------------------------------------------------------------------
// Players: dungeons, loot, ganking, duels, and the two refusals
// -----------------------------------------------------------------------

class BotSocialPlayerScript : public PlayerScript
{
public:
    BotSocialPlayerScript()
        : PlayerScript("BotSocialPlayerScript",
                       {PLAYERHOOK_ON_LOGIN,
                        PLAYERHOOK_ON_MAP_CHANGED,
                        PLAYERHOOK_ON_PVP_KILL,
                        PLAYERHOOK_ON_DUEL_END,
                        PLAYERHOOK_ON_PLAYER_RESURRECT,
                        PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM,
                        PLAYERHOOK_CAN_GROUP_INVITE,
                        PLAYERHOOK_CAN_GROUP_ACCEPT}) {}

    void OnPlayerLogin(Player* player) override
    {
        BotSocial::OnLogin(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!BotSocial::Cfg().enable || !player)
            return;

        Map* map = player->GetMap();
        if (!map)
            return;

        // Walking into an instance together is the strongest single
        // signal we get: the group survived the queue and the travel.
        if (map->IsDungeon())
            BotSocial::OnDungeonEntered(player, map->GetId());
        else if (map->IsBattleground())
            BotSocial::OnBattlegroundEntered(player);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        BotSocial::OnPvpKill(killer, killed);
    }

    void OnPlayerDuelEnd(Player* winner, Player* loser,
                         DuelCompleteType type) override
    {
        // A duel that fizzled out is not a defeat.
        if (type != DUEL_WON)
            return;

        BotSocial::OnDuelFinished(winner, loser);
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/,
                           bool& /*applySickness*/) override
    {
        BotSocial::OnResurrected(player);
    }

    void OnPlayerGroupRollRewardItem(Player* player, Item* item,
                                     uint32 /*count*/, RollVote voteType,
                                     Roll* /*roll*/) override
    {
        BotSocial::OnLootRoll(player, item, uint32(voteType));
    }

    bool OnPlayerCanGroupInvite(Player* player,
                                std::string& membername) override
    {
        return !BotSocial::WouldRefuseInvite(player, membername);
    }

    bool OnPlayerCanGroupAccept(Player* player, Group* group) override
    {
        return !BotSocial::WouldRefuseGroup(player, group);
    }
};

// -----------------------------------------------------------------------
// .social - so we are not flying blind
// -----------------------------------------------------------------------

class BotSocialCommandScript : public CommandScript
{
public:
    BotSocialCommandScript() : CommandScript("BotSocialCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable socialTable =
        {
            { "bonds",   HandleBondsCommand,   SEC_GAMEMASTER, Console::No  },
            { "grudges", HandleGrudgesCommand, SEC_GAMEMASTER, Console::No  },
            { "who",     HandleWhoCommand,     SEC_GAMEMASTER, Console::No  },
            { "stats",   HandleStatsCommand,   SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "social", socialTable },
        };

        return commandTable;
    }

private:
    static bool Resolve(ChatHandler* handler, std::string& name,
                        uint32& guid)
    {
        if (name.empty())
        {
            Player* target = handler->getSelectedPlayer();
            if (!target)
            {
                handler->SendSysMessage(
                    "Ziel anvisieren oder einen Namen angeben.");
                return false;
            }
            name = target->GetName();
        }

        if (!BotSocial::ResolveCharacter(name, guid))
        {
            handler->PSendSysMessage("Charakter '{}' nicht gefunden.", name);
            return false;
        }

        return true;
    }

    static void PrintRows(ChatHandler* handler,
                          std::vector<BotSocial::BondRow> const& rows)
    {
        for (BotSocial::BondRow const& r : rows)
        {
            std::string mark;
            if (r.ignored)
                mark = " [ignoriert]";
            else if (r.befriended)
                mark = " [Freund]";

            std::string why;
            if (r.grudges && !r.lastGrudge.empty())
                why = ", zuletzt: " + r.lastGrudge;

            handler->PSendSysMessage(
                "  {}{} - Wert {}, {}x gruppiert, {} Min, {} Instanzen{}",
                r.otherName, mark, r.affinity, r.timesGrouped,
                r.minutesTogether, r.dungeons, why);
        }
    }

public:
    static bool HandleBondsCommand(ChatHandler* handler, Tail nameArg)
    {
        std::string name(nameArg);
        uint32 guid = 0;
        if (!Resolve(handler, name, guid))
            return true;

        std::vector<BotSocial::BondRow> rows = BotSocial::TopBonds(guid, 12);
        if (rows.empty())
        {
            handler->PSendSysMessage("{} kennt noch niemanden.", name);
            return true;
        }

        handler->PSendSysMessage("Wen {} mag:", name);
        PrintRows(handler, rows);
        return true;
    }

    static bool HandleGrudgesCommand(ChatHandler* handler, Tail nameArg)
    {
        std::string name(nameArg);
        uint32 guid = 0;
        if (!Resolve(handler, name, guid))
            return true;

        std::vector<BotSocial::BondRow> rows =
            BotSocial::WorstBonds(guid, 12);

        // Only the ones that are actually negative.
        std::vector<BotSocial::BondRow> bad;
        for (BotSocial::BondRow const& r : rows)
            if (r.affinity < 0)
                bad.push_back(r);

        if (bad.empty())
        {
            handler->PSendSysMessage("{} ist mit allen im Reinen.", name);
            return true;
        }

        handler->PSendSysMessage("Mit wem {} ein Problem hat:", name);
        PrintRows(handler, bad);
        return true;
    }

    static bool HandleWhoCommand(ChatHandler* handler, Tail nameArg)
    {
        std::string name(nameArg);
        uint32 guid = 0;
        if (!Resolve(handler, name, guid))
            return true;

        BotSocial::ProfileRow p = BotSocial::GetProfile(guid);
        if (!p.found)
        {
            handler->PSendSysMessage(
                "Für {} gibt es noch kein Profil - vermutlich seit dem "
                "Start nicht eingeloggt.", name);
            return true;
        }

        handler->PSendSysMessage("{}:", name);
        handler->PSendSysMessage("  Spielertyp:  {}", p.archetype);
        handler->PSendSysMessage("  Spielstaerke: {} von 5", p.skillTier);
        handler->PSendSysMessage("  Geselligkeit: {}", p.sociability);
        handler->PSendSysMessage("  Ehrgeiz:      {}", p.ambition);
        handler->PSendSysMessage("  Ruf:          {}", p.reputation);
        return true;
    }

    static bool HandleStatsCommand(ChatHandler* handler)
    {
        BotSocial::Stats s = BotSocial::Snapshot();

        handler->PSendSysMessage("mod-bot-social:");
        handler->PSendSysMessage("  Bindungen im Speicher: {}", s.bondRows);
        handler->PSendSysMessage("  Freundschaften:        {}", s.friendships);
        handler->PSendSysMessage("  offene Rechnungen:     {}", s.grudges);
        handler->PSendSysMessage("  Ignorierungen:         {}", s.ignores);
        handler->PSendSysMessage("  Gilden beobachtet:     {}", s.guildsTracked);
        handler->PSendSysMessage("  Mitglieder geworben:   {}", s.recruited);
        handler->PSendSysMessage("  Gildenspaltungen:      {}", s.schisms);
        handler->PSendSysMessage("  ungeschrieben:         {}", s.pendingWrites);
        return true;
    }
};

// -----------------------------------------------------------------------

void AddSC_mod_bot_social()
{
    new BotSocialWorldScript();
    new BotSocialGroupScript();
    new BotSocialGuildScript();
    new BotSocialPlayerScript();
    new BotSocialCommandScript();
}
