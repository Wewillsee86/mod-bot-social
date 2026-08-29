/*
 * mod-bot-social - implementation.
 *
 * The whole social graph is held in memory and the database is only
 * its backup. That is deliberate: the working set is tiny (a Dunbar
 * cap of 50 edges times a few hundred bots is a couple of megabytes)
 * while the access pattern - "bump this edge", "who does X like best"
 * - would otherwise mean a query on the world tick. Clique detection
 * and gossip need to walk neighbours, and walking a hash map costs
 * nothing.
 */

#include "BotSocial.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BotSocial
{
namespace
{
    Config _cfg;

    // ---- the resident graph -------------------------------------------

    struct Edge
    {
        int32  affinity   = 0;
        uint32 encounters = 0;
        uint32 grouped    = 0;
        uint32 minutes    = 0;
        uint32 dungeons   = 0;
        uint32 battles    = 0;
        uint32 grudges    = 0;
        int32  favours    = 0;
        bool   befriended = false;
        bool   ignored    = false;
        bool   dirty      = false;
        std::string lastGrudge;
    };

    using Neighbours = std::unordered_map<uint32, Edge>;

    std::unordered_map<uint32, Neighbours> _graph;
    std::recursive_mutex                   _graphMutex;

    // Real players are treated gently - see the guard rails in the
    // config. We learn who they are on login.
    std::unordered_set<uint32> _humans;

    struct Profile
    {
        std::string archetype = "unset";
        uint32 skillTier   = 2;
        uint32 sociability = 50;
        uint32 ambition    = 50;
        int32  reputation  = 0;
        bool   dirty       = false;
    };

    std::unordered_map<uint32, Profile> _profiles;

    // ---- who sits in which group, since when --------------------------
    std::map<uint32, std::map<uint32, time_t>> _groupSince;
    std::mutex                                 _groupMutex;

    // ---- timers --------------------------------------------------------
    uint32 _flushTimer   = 0;
    uint32 _recruitTimer = 0;
    uint32 _rankTimer    = 0;
    uint32 _decayTimer   = 0;
    uint32 _schismTimer  = 0;
    uint32 _decayRound   = 0;

    uint64 _schismTotal    = 0;
    time_t _lastSchism     = 0;

    std::mt19937& Rng()
    {
        static std::mt19937 engine{std::random_device{}()};
        return engine;
    }

    uint32 RollBetween(uint32 lo, uint32 hi)
    {
        if (hi <= lo)
            return lo;
        std::uniform_int_distribution<uint32> dist(lo, hi);
        return dist(Rng());
    }

    ObjectGuid MakeGuid(uint32 low)
    {
        return ObjectGuid::Create<HighGuid::Player>(low);
    }

    std::string Escape(std::string const& in)
    {
        std::string out = in;
        CharacterDatabase.EscapeString(out);
        return out;
    }

    bool IsHuman(uint32 guid)
    {
        return _humans.find(guid) != _humans.end();
    }

    // ---- the one place edges are written -------------------------------

    struct Touch
    {
        int32  affinity   = 0;
        uint32 grouped    = 0;
        uint32 minutes    = 0;
        uint32 dungeons   = 0;
        uint32 battles    = 0;
        uint32 grudges    = 0;
        int32  favours    = 0;
        bool   countsAsMeeting = true;
        std::string grudgeKind;

        // Trace: non-empty means Apply() logs the EFFECTIVE award (after
        // the first-impression multiplier) into bot_social_event. Grudge
        // paths leave this empty - they already log themselves, a second
        // row would double-count them.
        std::string kind;

        // Ort des Ereignisses, wenn ein Player greifbar war. 0 heisst
        // ehrlich "unbekannt" (Minuten-Tick, Gruppenbeitritt).
        uint32 mapId  = 0;
        uint32 zoneId = 0;
    };

    void LogEvent(uint32 actor, uint32 target, std::string const& kind,
                  std::string const& detail, int32 weight,
                  uint32 mapId = 0, uint32 zoneId = 0);

    // Aktives GM-Flag (.gm on): Testen soll keine Punkte und keine
    // Protokollzeilen erzeugen.
    bool IgnoredActor(Player* player)
    {
        return _cfg.ignoreGameMasters && player && player->IsGameMaster();
    }

    // Applies one side of an interaction. Callers pass a different
    // Touch per direction, because liking is not symmetric.
    void Apply(uint32 from, uint32 to, Touch const& t)
    {
        if (!from || !to || from == to)
            return;

        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        Edge& e = _graph[from][to];

        // The first few meetings leave a disproportionate mark. This is
        // why bailing on someone the first time you group with them is
        // so much worse than doing it on the twentieth.
        int32 weight = t.affinity;
        if (e.encounters < _cfg.firstImpressions)
            weight *= _cfg.firstImpressionMul;

        e.affinity += weight;
        e.grouped  += t.grouped;
        e.minutes  += t.minutes;
        e.dungeons += t.dungeons;
        e.battles  += t.battles;
        e.grudges  += t.grudges;
        e.favours  += t.favours;

        if (t.countsAsMeeting)
            ++e.encounters;

        if (!t.grudgeKind.empty())
            e.lastGrudge = t.grudgeKind;

        e.dirty = true;

        // Trace: write the effective award to bot_social_event, so the
        // stored affinity can be recomputed from the log. Uses the same
        // async Execute as every other write - no extra latency here.
        if (_cfg.traceEnable && !t.kind.empty()
            && std::abs(weight) >= _cfg.traceMinWeight)
            LogEvent(from, to, t.kind, "", weight, t.mapId, t.zoneId);
    }

    void LogEvent(uint32 actor, uint32 target, std::string const& kind,
                  std::string const& detail, int32 weight,
                  uint32 mapId, uint32 zoneId)
    {
        CharacterDatabase.Execute(
            "INSERT INTO bot_social_event "
            "(actor_guid, target_guid, kind, detail, weight, "
            "map_id, zone_id) "
            "VALUES ({}, {}, '{}', '{}', {}, {}, {})",
            actor, target, Escape(kind), Escape(detail), weight,
            mapId, zoneId);
    }

    void BumpReputation(uint32 guid, int32 delta)
    {
        if (!_cfg.reputationEnable || !delta)
            return;

        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        Profile& p = _profiles[guid];
        p.reputation += delta;
        p.dirty = true;
    }

    std::vector<uint32> GroupMemberLowGuids(Group* group)
    {
        std::vector<uint32> out;
        if (!group)
            return out;

        for (auto const& slot : group->GetMemberSlots())
            out.push_back(slot.guid.GetCounter());

        return out;
    }

    int32 AffinityOf(uint32 from, uint32 to)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        auto it = _graph.find(from);
        if (it == _graph.end())
            return 0;

        auto jt = it->second.find(to);
        return jt == it->second.end() ? 0 : jt->second.affinity;
    }

    // ---- conflict helper ----------------------------------------------

    // One bot wrongs the whole rest of a group. Everyone present loses
    // affection for the culprit; the culprit itself loses nothing,
    // because people rarely think worse of themselves.
    void GroupGrudge(Group* group, uint32 culprit, int32 amount,
                     std::string const& kind, std::string const& detail,
                     uint32 mapId = 0, uint32 zoneId = 0)
    {
        if (!_cfg.conflictEnable || !group || !amount)
            return;

        Touch t;
        t.affinity   = -amount;
        t.grudges    = 1;
        t.grudgeKind = kind;
        t.countsAsMeeting = false;

        uint32 victims = 0;
        for (uint32 other : GroupMemberLowGuids(group))
        {
            if (other == culprit)
                continue;

            Apply(other, culprit, t);
            ++victims;
        }

        if (!victims)
            return;

        BumpReputation(culprit, -_cfg.reputationPerGrudge);
        LogEvent(culprit, 0, kind, detail, -amount, mapId, zoneId);

        if (_cfg.debug)
            LOG_INFO("module", "BotSocial: {} upset {} group members ({})",
                     culprit, victims, kind);
    }

    // ---- persistence ---------------------------------------------------

    void LoadGraph()
    {
        QueryResult res = CharacterDatabase.Query(
            "SELECT bot_guid, other_guid, affinity, encounters, "
            "times_grouped, minutes_together, dungeons_together, "
            "battles_together, grudge_events, favours_owed, "
            "befriended, ignored, last_grudge "
            "FROM bot_social_bond");

        uint32 loaded = 0;
        if (res)
        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);
            do
            {
                Field* f = res->Fetch();
                uint32 a = f[0].Get<uint32>();
                uint32 b = f[1].Get<uint32>();

                Edge& e = _graph[a][b];
                e.affinity   = f[2].Get<int32>();
                e.encounters = f[3].Get<uint32>();
                e.grouped    = f[4].Get<uint32>();
                e.minutes    = f[5].Get<uint32>();
                e.dungeons   = f[6].Get<uint32>();
                e.battles    = f[7].Get<uint32>();
                e.grudges    = f[8].Get<uint32>();
                e.favours    = f[9].Get<int32>();
                e.befriended = f[10].Get<uint8>() != 0;
                e.ignored    = f[11].Get<uint8>() != 0;
                e.lastGrudge = f[12].Get<std::string>();
                e.dirty      = false;
                ++loaded;
            } while (res->NextRow());
        }

        QueryResult pres = CharacterDatabase.Query(
            "SELECT bot_guid, archetype, skill_tier, sociability, "
            "ambition, reputation FROM bot_social_profile");

        uint32 profiles = 0;
        if (pres)
        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);
            do
            {
                Field* f = pres->Fetch();
                Profile& p = _profiles[f[0].Get<uint32>()];
                p.archetype   = f[1].Get<std::string>();
                p.skillTier   = f[2].Get<uint8>();
                p.sociability = f[3].Get<uint8>();
                p.ambition    = f[4].Get<uint8>();
                p.reputation  = f[5].Get<int32>();
                p.dirty       = false;
                ++profiles;
            } while (pres->NextRow());
        }

        LOG_INFO("module",
                 "mod-bot-social: {} Bindungen und {} Profile geladen",
                 loaded, profiles);
    }

    void FlushGraph()
    {
        std::vector<std::string> statements;

        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);

            for (auto& node : _graph)
            {
                for (auto& edge : node.second)
                {
                    Edge& e = edge.second;
                    if (!e.dirty)
                        continue;

                    std::ostringstream sql;
                    sql << "INSERT INTO bot_social_bond "
                           "(bot_guid, other_guid, affinity, encounters, "
                           "times_grouped, minutes_together, "
                           "dungeons_together, battles_together, "
                           "grudge_events, favours_owed, befriended, "
                           "ignored, last_grudge) VALUES ("
                        << node.first << ", " << edge.first << ", "
                        << e.affinity << ", " << e.encounters << ", "
                        << e.grouped << ", " << e.minutes << ", "
                        << e.dungeons << ", " << e.battles << ", "
                        << e.grudges << ", " << e.favours << ", "
                        << (e.befriended ? 1 : 0) << ", "
                        << (e.ignored ? 1 : 0) << ", '"
                        << Escape(e.lastGrudge)
                        << "') ON DUPLICATE KEY UPDATE "
                           "affinity = " << e.affinity
                        << ", encounters = " << e.encounters
                        << ", times_grouped = " << e.grouped
                        << ", minutes_together = " << e.minutes
                        << ", dungeons_together = " << e.dungeons
                        << ", battles_together = " << e.battles
                        << ", grudge_events = " << e.grudges
                        << ", favours_owed = " << e.favours
                        << ", befriended = " << (e.befriended ? 1 : 0)
                        << ", ignored = " << (e.ignored ? 1 : 0)
                        << ", last_grudge = '" << Escape(e.lastGrudge)
                        << "', last_seen = NOW()";

                    statements.push_back(sql.str());
                    e.dirty = false;
                }
            }

            for (auto& entry : _profiles)
            {
                Profile& p = entry.second;
                if (!p.dirty)
                    continue;

                std::ostringstream sql;
                sql << "INSERT INTO bot_social_profile "
                       "(bot_guid, archetype, skill_tier, sociability, "
                       "ambition, reputation) VALUES ("
                    << entry.first << ", '" << Escape(p.archetype) << "', "
                    << p.skillTier << ", " << p.sociability << ", "
                    << p.ambition << ", " << p.reputation
                    << ") ON DUPLICATE KEY UPDATE "
                       "archetype = '" << Escape(p.archetype)
                    << "', skill_tier = " << p.skillTier
                    << ", sociability = " << p.sociability
                    << ", ambition = " << p.ambition
                    << ", reputation = " << p.reputation;

                statements.push_back(sql.str());
                p.dirty = false;
            }
        }

        if (statements.empty())
            return;

        CharacterDatabaseTransaction trans =
            CharacterDatabase.BeginTransaction();

        for (std::string const& s : statements)
            trans->Append(s.c_str());

        CharacterDatabase.CommitTransaction(trans);

        if (_cfg.debug)
            LOG_INFO("module", "BotSocial: {} Zeilen geschrieben",
                     statements.size());
    }

    // ---- Dunbar --------------------------------------------------------

    // Nobody keeps track of eight hundred people. When a bot's circle
    // grows past the cap, the weakest ties fall out of memory - unless
    // they are marked, because you do not forget your friends or the
    // person you swore never to group with again.
    void EnforceDunbar()
    {
        if (!_cfg.maxBondsPerBot)
            return;

        std::vector<std::pair<uint32, uint32>> dropped;

        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);

            for (auto& node : _graph)
            {
                if (node.second.size() <= _cfg.maxBondsPerBot)
                    continue;

                // Friends and people you have sworn off are never
                // forgotten - they eat into the budget instead.
                std::vector<std::pair<uint32, int32>> ranked;
                ranked.reserve(node.second.size());

                size_t marked = 0;
                for (auto const& edge : node.second)
                {
                    if (edge.second.befriended || edge.second.ignored)
                    {
                        ++marked;
                        continue;
                    }
                    ranked.emplace_back(edge.first,
                                        std::abs(edge.second.affinity));
                }

                size_t budget = marked >= _cfg.maxBondsPerBot
                    ? 0 : _cfg.maxBondsPerBot - marked;

                if (ranked.size() <= budget)
                    continue;

                std::sort(ranked.begin(), ranked.end(),
                          [](std::pair<uint32, int32> const& x,
                             std::pair<uint32, int32> const& y)
                          { return x.second > y.second; });

                for (size_t i = budget; i < ranked.size(); ++i)
                {
                    node.second.erase(ranked[i].first);
                    dropped.emplace_back(node.first, ranked[i].first);
                }
            }
        }

        if (dropped.empty())
            return;

        CharacterDatabaseTransaction trans =
            CharacterDatabase.BeginTransaction();

        for (auto const& d : dropped)
        {
            std::ostringstream sql;
            sql << "DELETE FROM bot_social_bond WHERE bot_guid = "
                << d.first << " AND other_guid = " << d.second;
            trans->Append(sql.str().c_str());
        }

        CharacterDatabase.CommitTransaction(trans);

        if (_cfg.debug)
            LOG_INFO("module", "BotSocial: {} schwache Bindungen vergessen",
                     dropped.size());
    }

    // ---- decay ---------------------------------------------------------

    // Friendships fade when you stop seeing each other. Grudges fade
    // too, but far more slowly - which is exactly how people work.
    void DecayTick()
    {
        ++_decayRound;

        bool decayNegativeNow =
            _cfg.decayNegativeDivisor == 0 ||
            (_decayRound % _cfg.decayNegativeDivisor) == 0;

        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        for (auto& node : _graph)
        {
            bool nodeIsHuman = IsHuman(node.first);

            for (auto& edge : node.second)
            {
                Edge& e = edge.second;

                if (e.affinity > 0)
                {
                    if (_cfg.decayPositive <= 0)
                        continue;
                    e.affinity = std::max(0,
                        e.affinity - _cfg.decayPositive);
                    e.dirty = true;
                }
                else if (e.affinity < 0)
                {
                    // A grudge against the only human on the server
                    // fades faster, on purpose. Being frozen out by
                    // hundreds of bots is not realism, it is a broken
                    // game.
                    bool aboutHuman = IsHuman(edge.first);
                    bool tick = decayNegativeNow;
                    if (aboutHuman && _cfg.playerGrudgeSpeedup > 1)
                        tick = true;

                    if (!tick || _cfg.decayNegative <= 0)
                        continue;

                    int32 step = _cfg.decayNegative;
                    if (aboutHuman)
                        step *= int32(_cfg.playerGrudgeSpeedup);

                    e.affinity = std::min(0, e.affinity + step);
                    e.dirty = true;

                    // Forgiveness lifts the ignore, but not the memory.
                    if (e.ignored && e.affinity > _cfg.ignoreThreshold)
                        e.ignored = false;
                }

                (void)nodeIsHuman;
            }
        }
    }

    // ---- friends and ignores -------------------------------------------

    void ApplySocialLists()
    {
        std::vector<std::pair<uint32, uint32>> toFriend;
        std::vector<std::pair<uint32, uint32>> toIgnore;

        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);

            for (auto& node : _graph)
            {
                for (auto& edge : node.second)
                {
                    Edge& e = edge.second;

                    if (_cfg.friendsEnable && !e.befriended
                        && e.affinity >= _cfg.friendsThreshold)
                    {
                        toFriend.emplace_back(node.first, edge.first);
                    }

                    if (_cfg.ignoreEnable && !e.ignored
                        && e.affinity <= _cfg.ignoreThreshold)
                    {
                        // The player never gets frozen out unless you
                        // explicitly allow it.
                        if (IsHuman(edge.first)
                            && !_cfg.playerCanBeIgnored)
                            continue;

                        toIgnore.emplace_back(node.first, edge.first);
                    }
                }
            }
        }

        for (auto const& pair : toFriend)
        {
            Player* owner = ObjectAccessor::FindPlayer(MakeGuid(pair.first));
            if (!owner || !owner->IsInWorld())
                continue;

            PlayerSocial* social = owner->GetSocial();
            if (!social)
                continue;

            ObjectGuid other = MakeGuid(pair.second);
            if (!social->HasFriend(other))
                social->AddToSocialList(other, SOCIAL_FLAG_FRIEND);

            std::lock_guard<std::recursive_mutex> guard(_graphMutex);
            Edge& e = _graph[pair.first][pair.second];
            e.befriended = true;
            e.dirty = true;
        }

        for (auto const& pair : toIgnore)
        {
            Player* owner = ObjectAccessor::FindPlayer(MakeGuid(pair.first));
            if (!owner || !owner->IsInWorld())
                continue;

            PlayerSocial* social = owner->GetSocial();
            if (!social)
                continue;

            social->AddToSocialList(MakeGuid(pair.second),
                                    SOCIAL_FLAG_IGNORED);

            std::lock_guard<std::recursive_mutex> guard(_graphMutex);
            Edge& e = _graph[pair.first][pair.second];
            e.ignored = true;
            e.dirty = true;

            LogEvent(pair.first, pair.second, "ignore", e.lastGrudge,
                     e.affinity);

            if (_cfg.debug)
                LOG_INFO("module", "BotSocial: {} ignoriert {} ({})",
                         pair.first, pair.second, e.lastGrudge);
        }
    }

    // ---- archetypes -----------------------------------------------------

    // How people actually spent their time in WotLK. The weights are a
    // rough head count of a mid-size realm, not a balance decision.
    struct ArchetypeDef
    {
        char const* name;
        uint32 weight;
        uint32 sociabilityLo;
        uint32 sociabilityHi;
        uint32 ambitionLo;
        uint32 ambitionHi;
    };

    ArchetypeDef const kArchetypes[] = {
        { "questor",    24, 30, 70, 20, 55 },  // levels, reads quests
        { "dungeoneer", 18, 55, 90, 40, 75 },  // lives in the finder
        { "raider",     10, 60, 95, 70, 100 }, // wants progress, impatient
        { "pvper",      12, 35, 80, 45, 85 },  // battlegrounds
        { "gatherer",   14, 15, 50, 25, 60 },  // professions, auctions
        { "casual",     16, 25, 65, 10, 40 },  // on now and then
        { "loner",       6,  0, 20,  5, 45 },  // never joins a guild
    };

    // Which pairings rub each other the wrong way.
    bool Clashes(std::string const& a, std::string const& b)
    {
        if (a == "raider" && b == "casual") return true;
        if (a == "casual" && b == "raider") return true;
        if (a == "loner")                   return true;
        if (b == "loner")                   return true;
        if (a == "pvper" && b == "gatherer") return true;
        if (a == "gatherer" && b == "pvper") return true;
        return false;
    }

    Profile& EnsureProfile(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        auto it = _profiles.find(guid);
        if (it != _profiles.end())
            return it->second;

        uint32 total = 0;
        for (ArchetypeDef const& a : kArchetypes)
            total += a.weight;

        uint32 roll = RollBetween(1, total);
        ArchetypeDef const* chosen = &kArchetypes[0];
        for (ArchetypeDef const& a : kArchetypes)
        {
            if (roll <= a.weight)
            {
                chosen = &a;
                break;
            }
            roll -= a.weight;
        }

        Profile& p = _profiles[guid];
        p.archetype   = chosen->name;
        p.sociability = RollBetween(chosen->sociabilityLo,
                                    chosen->sociabilityHi);
        p.ambition    = RollBetween(chosen->ambitionLo,
                                    chosen->ambitionHi);
        // 1 = poor, 5 = very good. Most people are average.
        uint32 tierRoll = RollBetween(1, 100);
        p.skillTier = tierRoll <= 10 ? 1
                    : tierRoll <= 30 ? 2
                    : tierRoll <= 70 ? 3
                    : tierRoll <= 92 ? 4 : 5;
        p.reputation = 0;
        p.dirty = true;

        return p;
    }

    std::string ArchetypeOf(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        auto it = _profiles.find(guid);
        return it == _profiles.end() ? std::string("unset")
                                     : it->second.archetype;
    }

    int32 ReputationOf(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        auto it = _profiles.find(guid);
        return it == _profiles.end() ? 0 : it->second.reputation;
    }

    // ---- guild recruitment ---------------------------------------------

    std::vector<uint32> GuildMemberGuids(uint32 guildId)
    {
        std::vector<uint32> out;

        QueryResult res = CharacterDatabase.Query(
            "SELECT guid FROM guild_member WHERE guildid = {}", guildId);

        if (!res)
            return out;

        do
        {
            out.push_back(res->Fetch()[0].Get<uint32>());
        } while (res->NextRow());

        return out;
    }

    uint32 PickRecruit(Guild* guild, TeamId team)
    {
        std::vector<uint32> members = GuildMemberGuids(guild->GetId());
        if (members.empty())
            return 0;

        std::vector<uint32> candidates;

        WorldSessionMgr::SessionMap const& sessions =
            sWorldSessionMgr->GetAllSessions();

        for (auto const& pair : sessions)
        {
            WorldSession* session = pair.second;
            if (!session)
                continue;

            Player* p = session->GetPlayer();
            if (!p || !p->IsInWorld() || !IsBot(p))
                continue;
            if (p->GetGuildId() || p->GetTeamId() != team)
                continue;

            uint32 low = p->GetGUID().GetCounter();

            // Loners stay out. Some people simply do not join things.
            if (ArchetypeOf(low) == "loner")
                continue;

            // A bad name travels ahead of you.
            if (ReputationOf(low) < _cfg.recruitMinReputation)
                continue;

            candidates.push_back(low);
        }

        if (candidates.empty())
            return 0;

        uint32 best = 0;
        int32  bestScore = _cfg.recruitMinAffinity - 1;
        int32  worstFeeling = 0;

        for (uint32 c : candidates)
        {
            int32 score = 0;
            worstFeeling = 0;

            for (uint32 m : members)
            {
                int32 a = AffinityOf(c, m);
                score = std::max(score, a);
                worstFeeling = std::min(worstFeeling, AffinityOf(m, c));
            }

            // Somebody in there cannot stand this candidate.
            if (worstFeeling <= _cfg.avoidThreshold)
                continue;

            if (score > bestScore)
            {
                bestScore = score;
                best = c;
            }
        }

        if (best)
            return best;

        if (!_cfg.recruitStrangers)
            return 0;

        // Nobody suitable is known. Take a stranger, but not one the
        // guild already dislikes.
        for (uint32 c : candidates)
        {
            bool disliked = false;
            for (uint32 m : members)
            {
                if (AffinityOf(m, c) <= _cfg.avoidThreshold)
                {
                    disliked = true;
                    break;
                }
            }
            if (!disliked)
                return c;
        }

        return 0;
    }

    void RecruitTick()
    {
        if (!_cfg.recruitEnable)
            return;

        QueryResult res = CharacterDatabase.Query(
            "SELECT guild_id, target_size FROM bot_social_guild "
            "WHERE last_recruit IS NULL "
            "   OR last_recruit < DATE_SUB(NOW(), INTERVAL {} SECOND) "
            "ORDER BY RAND() LIMIT {}",
            _cfg.recruitCooldown, _cfg.recruitPerTick);

        if (!res)
            return;

        do
        {
            Field* f = res->Fetch();
            uint32 guildId = f[0].Get<uint32>();
            uint32 target  = f[1].Get<uint32>();

            Guild* guild = sGuildMgr->GetGuildById(guildId);
            if (!guild)
            {
                CharacterDatabase.Execute(
                    "DELETE FROM bot_social_guild WHERE guild_id = {}",
                    guildId);
                continue;
            }

            if (guild->GetMemberSize() >= target)
                continue;

            Player* leader =
                ObjectAccessor::FindPlayer(guild->GetLeaderGUID());
            if (!leader || !leader->IsInWorld())
                continue;

            uint32 recruit = PickRecruit(guild, leader->GetTeamId());
            if (!recruit)
                continue;

            Player* rp = ObjectAccessor::FindPlayer(MakeGuid(recruit));
            if (!rp || !rp->IsInWorld())
                continue;

            bool joined = guild->AddMember(rp->GetGUID());

            // The cooldown is spent on the attempt, not on the success.
            // Otherwise a guild that cannot take anyone gets rescanned
            // on every single tick.
            CharacterDatabase.Execute(
                "UPDATE bot_social_guild SET last_recruit = NOW(), "
                "recruited = recruited + {} WHERE guild_id = {}",
                joined ? 1 : 0, guildId);

            if (joined)
            {

                LogEvent(recruit, 0, "guild_join", guild->GetName(), 0);

                if (_cfg.debug)
                    LOG_INFO("module",
                             "BotSocial: {} trat '{}' bei ({}/{})",
                             rp->GetName(), guild->GetName(),
                             guild->GetMemberSize(), target);
            }
        } while (res->NextRow());
    }

    // ---- rank progression ------------------------------------------------

    void RankTick()
    {
        if (!_cfg.ranksEnable)
            return;

        WorldSessionMgr::SessionMap const& sessions =
            sWorldSessionMgr->GetAllSessions();

        for (auto const& pair : sessions)
        {
            WorldSession* session = pair.second;
            if (!session)
                continue;

            Player* p = session->GetPlayer();
            if (!p || !p->IsInWorld() || !IsBot(p))
                continue;

            uint32 guildId = p->GetGuildId();
            if (!guildId)
                continue;

            Guild* guild = sGuildMgr->GetGuildById(guildId);
            if (!guild)
                continue;

            Guild::Member* member = guild->GetMember(p->GetGUID());
            if (!member)
                continue;

            uint8 rank = member->GetRankId();
            if (rank <= 2)
                continue;

            uint32 low = p->GetGUID().GetCounter();

            int32 total = 0;
            for (uint32 m : GuildMemberGuids(guildId))
                total += AffinityOf(low, m);

            if (total < _cfg.rankPromoteAffinity)
                continue;

            if (guild->ChangeMemberRank(p->GetGUID(), rank - 1))
            {
                LogEvent(low, 0, "promoted", guild->GetName(), rank - 1);

                if (_cfg.debug)
                    LOG_INFO("module",
                             "BotSocial: {} in '{}' auf Rang {} befördert",
                             p->GetName(), guild->GetName(), rank - 1);
            }
        }
    }

    // ---- the schism -------------------------------------------------------

    // A clique inside a guild that is close to each other and cold
    // towards the guild master walks out together. We do not found the
    // new guild ourselves - once they are guildless, mod-playerbots'
    // own charter logic picks them up. Causing the drama is our job;
    // the paperwork is not.
    void SchismTick()
    {
        if (!_cfg.schismEnable)
            return;

        time_t now = time(nullptr);
        if (_lastSchism &&
            uint32(now - _lastSchism) < _cfg.schismServerCooldown)
            return;

        QueryResult res = CharacterDatabase.Query(
            "SELECT guild_id FROM bot_social_guild "
            "WHERE last_schism IS NULL "
            "   OR last_schism < DATE_SUB(NOW(), INTERVAL {} SECOND) "
            "ORDER BY RAND() LIMIT 8",
            _cfg.schismGuildCooldown);

        if (!res)
            return;

        do
        {
            uint32 guildId = res->Fetch()[0].Get<uint32>();

            Guild* guild = sGuildMgr->GetGuildById(guildId);
            if (!guild || guild->GetMemberSize() < _cfg.schismMinGuildSize)
                continue;

            uint32 masterLow = guild->GetLeaderGUID().GetCounter();
            std::vector<uint32> members = GuildMemberGuids(guildId);

            // Find the most plausible ringleader: dislikes the guild
            // master, is well connected inside the guild, and is
            // ambitious enough to want their own tabard.
            uint32 ringleader = 0;
            int32  bestClique = _cfg.schismCliqueBond - 1;

            for (uint32 m : members)
            {
                if (m == masterLow || IsHuman(m))
                    continue;
                if (AffinityOf(m, masterLow) > _cfg.schismLeaderDislike)
                    continue;

                Profile& prof = EnsureProfile(m);
                if (prof.ambition < 60)
                    continue;

                int32 clique = 0;
                for (uint32 o : members)
                    if (o != m && o != masterLow)
                        clique += std::max(0, AffinityOf(m, o));

                if (clique > bestClique)
                {
                    bestClique = clique;
                    ringleader = m;
                }
            }

            if (!ringleader)
                continue;

            // Who goes with them: the people who like the ringleader
            // more than they like the guild master.
            std::vector<std::pair<uint32, int32>> loyal;
            for (uint32 m : members)
            {
                if (m == ringleader || m == masterLow || IsHuman(m))
                    continue;

                int32 toRing   = AffinityOf(m, ringleader);
                int32 toMaster = AffinityOf(m, masterLow);
                if (toRing > 0 && toRing > toMaster)
                    loyal.emplace_back(m, toRing);
            }

            if (loyal.size() < _cfg.schismMinFollowers)
                continue;

            std::sort(loyal.begin(), loyal.end(),
                      [](std::pair<uint32, int32> const& a,
                         std::pair<uint32, int32> const& b)
                      { return a.second > b.second; });

            if (loyal.size() > _cfg.schismMaxFollowers)
                loyal.resize(_cfg.schismMaxFollowers);

            guild->DeleteMember(MakeGuid(ringleader));
            for (auto const& l : loyal)
                guild->DeleteMember(MakeGuid(l.first));

            CharacterDatabase.Execute(
                "UPDATE bot_social_guild SET last_schism = NOW() "
                "WHERE guild_id = {}", guildId);

            std::ostringstream detail;
            detail << guild->GetName() << " (" << (loyal.size() + 1)
                   << " ausgetreten)";
            LogEvent(ringleader, masterLow, "schism", detail.str(),
                     int32(loyal.size() + 1));

            _lastSchism = now;
            ++_schismTotal;

            LOG_INFO("module",
                     "BotSocial: Gildenspaltung in '{}' - {} nahm {} Leute mit",
                     guild->GetName(), ringleader, loyal.size());

            return;
        } while (res->NextRow());
    }
} // anonymous namespace

// ---------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------

Config const& Cfg()
{
    return _cfg;
}

void LoadConfig()
{
    _cfg.enable = sConfigMgr->GetOption<bool>("BotSocial.Enable", false);
    _cfg.debug  = sConfigMgr->GetOption<bool>("BotSocial.DebugLog", false);

    _cfg.gainGroupJoin       = sConfigMgr->GetOption<int32>("BotSocial.Gain.GroupJoin", 2);
    _cfg.gainPerMinute       = sConfigMgr->GetOption<int32>("BotSocial.Gain.PerMinute", 1);
    _cfg.gainDungeon         = sConfigMgr->GetOption<int32>("BotSocial.Gain.Dungeon", 15);
    _cfg.gainBattleground    = sConfigMgr->GetOption<int32>("BotSocial.Gain.Battleground", 8);
    _cfg.gainResurrect       = sConfigMgr->GetOption<int32>("BotSocial.Gain.Resurrected", 12);
    _cfg.gainResurrector     = sConfigMgr->GetOption<int32>("BotSocial.Gain.Resurrector", 4);
    _cfg.maxMinutesPerStint  = sConfigMgr->GetOption<int32>("BotSocial.Gain.MaxMinutesPerStint", 60);

    _cfg.conflictEnable  = sConfigMgr->GetOption<bool>("BotSocial.Conflict.Enable", true);
    _cfg.lossKicked      = sConfigMgr->GetOption<int32>("BotSocial.Conflict.Kicked", 40);
    _cfg.lossKickedLfg   = sConfigMgr->GetOption<int32>("BotSocial.Conflict.KickedLfg", 55);
    _cfg.lossBailed      = sConfigMgr->GetOption<int32>("BotSocial.Conflict.Bailed", 25);
    _cfg.lossNinjaLoot   = sConfigMgr->GetOption<int32>("BotSocial.Conflict.NinjaLoot", 35);
    _cfg.lossGuildKick   = sConfigMgr->GetOption<int32>("BotSocial.Conflict.GuildKick", 60);
    _cfg.lossBankDrain   = sConfigMgr->GetOption<int32>("BotSocial.Conflict.BankDrain", 8);
    _cfg.bankDrainCopper = sConfigMgr->GetOption<uint32>("BotSocial.Conflict.BankDrainCopper", 500000);
    _cfg.lossGank        = sConfigMgr->GetOption<int32>("BotSocial.Conflict.Gank", 20);
    _cfg.gankLevelGap    = sConfigMgr->GetOption<uint32>("BotSocial.Conflict.GankLevelGap", 8);
    _cfg.lossDuel        = sConfigMgr->GetOption<int32>("BotSocial.Conflict.DuelLoss", 2);

    _cfg.firstImpressions   = sConfigMgr->GetOption<uint32>("BotSocial.FirstImpressions", 3);
    _cfg.firstImpressionMul = sConfigMgr->GetOption<int32>("BotSocial.FirstImpressionMultiplier", 2);
    _cfg.clashPenalty       = sConfigMgr->GetOption<int32>("BotSocial.ArchetypeClashPenalty", 2);

    _cfg.decayIntervalHours   = sConfigMgr->GetOption<uint32>("BotSocial.Decay.IntervalHours", 24);
    _cfg.decayPositive        = sConfigMgr->GetOption<int32>("BotSocial.Decay.Positive", 1);
    _cfg.decayNegative        = sConfigMgr->GetOption<int32>("BotSocial.Decay.Negative", 1);
    _cfg.decayNegativeDivisor = sConfigMgr->GetOption<uint32>("BotSocial.Decay.NegativeDivisor", 3);

    _cfg.maxBondsPerBot = sConfigMgr->GetOption<uint32>("BotSocial.MaxBondsPerBot", 50);

    _cfg.friendsEnable    = sConfigMgr->GetOption<bool>("BotSocial.Friends.Enable", true);
    _cfg.friendsThreshold = sConfigMgr->GetOption<int32>("BotSocial.Friends.Threshold", 60);
    _cfg.avoidEnable      = sConfigMgr->GetOption<bool>("BotSocial.Avoid.Enable", true);
    _cfg.avoidThreshold   = sConfigMgr->GetOption<int32>("BotSocial.Avoid.Threshold", -30);
    _cfg.ignoreEnable     = sConfigMgr->GetOption<bool>("BotSocial.Ignore.Enable", true);
    _cfg.ignoreThreshold  = sConfigMgr->GetOption<int32>("BotSocial.Ignore.Threshold", -90);

    _cfg.playerEnable        = sConfigMgr->GetOption<bool>("BotSocial.Player.Enable", true);
    _cfg.playerCanBeIgnored  = sConfigMgr->GetOption<bool>("BotSocial.Player.CanBeIgnored", false);
    _cfg.playerGrudgeSpeedup = sConfigMgr->GetOption<uint32>("BotSocial.Player.GrudgeFadeSpeedup", 2);

    _cfg.recruitEnable        = sConfigMgr->GetOption<bool>("BotSocial.Recruit.Enable", true);
    _cfg.recruitInterval      = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.Interval", 300);
    _cfg.recruitPerTick       = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.PerTick", 2);
    _cfg.recruitCooldown      = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.GuildCooldown", 900);
    _cfg.guildTargetMin       = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.TargetSizeMin", 12);
    _cfg.guildTargetMax       = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.TargetSizeMax", 45);

    // mod-playerbots flags a guild as full at AiPlayerbot.RandomBotGuildSizeMax and stops handing
    // it new bots. Recruiting beyond that only produces guilds the other module has written off,
    // so the roll range is clamped to that ceiling. Read straight from sConfigMgr: the value must
    // work whether or not mod-playerbots is compiled in.
    _cfg.guildCapacityCap = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.GuildCapacityCap", 0);
    if (!_cfg.guildCapacityCap)
        _cfg.guildCapacityCap = sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotGuildSizeMax", 0);

    if (_cfg.guildCapacityCap)
    {
        if (_cfg.guildTargetMax > _cfg.guildCapacityCap)
        {
            LOG_INFO("module",
                     "mod-bot-social: TargetSizeMax {} liegt ueber der Gildenkapazitaet {} - "
                     "auf {} begrenzt.",
                     _cfg.guildTargetMax, _cfg.guildCapacityCap, _cfg.guildCapacityCap);
            _cfg.guildTargetMax = _cfg.guildCapacityCap;
        }

        // A Min above the ceiling would make every guild roll the same clamped value and silently
        // drop the intended size spread, so pull it down too rather than inverting the range.
        if (_cfg.guildTargetMin > _cfg.guildTargetMax)
        {
            LOG_INFO("module",
                     "mod-bot-social: TargetSizeMin {} liegt ueber der begrenzten TargetSizeMax {} - "
                     "auf {} gesenkt.",
                     _cfg.guildTargetMin, _cfg.guildTargetMax, _cfg.guildTargetMax);
            _cfg.guildTargetMin = _cfg.guildTargetMax;
        }
    }
    _cfg.recruitMinAffinity   = sConfigMgr->GetOption<int32>("BotSocial.Recruit.MinAffinity", 25);
    _cfg.recruitMinReputation = sConfigMgr->GetOption<int32>("BotSocial.Recruit.MinReputation", -50);
    _cfg.recruitStrangers     = sConfigMgr->GetOption<bool>("BotSocial.Recruit.AllowStrangers", true);

    _cfg.ranksEnable         = sConfigMgr->GetOption<bool>("BotSocial.Ranks.Enable", true);
    _cfg.rankPromoteAffinity = sConfigMgr->GetOption<int32>("BotSocial.Ranks.PromoteAffinity", 150);
    _cfg.rankPromoteInterval = sConfigMgr->GetOption<uint32>("BotSocial.Ranks.Interval", 3600);

    _cfg.schismEnable         = sConfigMgr->GetOption<bool>("BotSocial.Schism.Enable", false);
    _cfg.schismInterval       = sConfigMgr->GetOption<uint32>("BotSocial.Schism.Interval", 3600);
    _cfg.schismServerCooldown = sConfigMgr->GetOption<uint32>("BotSocial.Schism.ServerCooldown", 604800);
    _cfg.schismGuildCooldown  = sConfigMgr->GetOption<uint32>("BotSocial.Schism.GuildCooldown", 2592000);
    _cfg.schismLeaderDislike  = sConfigMgr->GetOption<int32>("BotSocial.Schism.LeaderDislike", -40);
    _cfg.schismCliqueBond     = sConfigMgr->GetOption<int32>("BotSocial.Schism.CliqueBond", 120);
    _cfg.schismMinFollowers   = sConfigMgr->GetOption<uint32>("BotSocial.Schism.MinFollowers", 2);
    _cfg.schismMaxFollowers   = sConfigMgr->GetOption<uint32>("BotSocial.Schism.MaxFollowers", 6);
    _cfg.schismMinGuildSize   = sConfigMgr->GetOption<uint32>("BotSocial.Schism.MinGuildSize", 12);

    _cfg.reputationEnable    = sConfigMgr->GetOption<bool>("BotSocial.Reputation.Enable", true);
    _cfg.reputationPerGrudge = sConfigMgr->GetOption<int32>("BotSocial.Reputation.PerGrudge", 3);
    _cfg.reputationPerFavour = sConfigMgr->GetOption<int32>("BotSocial.Reputation.PerFavour", 1);

    _cfg.traceEnable        = sConfigMgr->GetOption<bool>("BotSocial.Trace.Enable", false);
    _cfg.traceMinWeight     = sConfigMgr->GetOption<int32>("BotSocial.Trace.MinWeight", 0);
    _cfg.traceRetentionDays = sConfigMgr->GetOption<uint32>("BotSocial.Trace.RetentionDays", 14);

    _cfg.flushInterval = sConfigMgr->GetOption<uint32>("BotSocial.FlushInterval", 60);

    _cfg.ignoreGameMasters = sConfigMgr->GetOption<bool>(
        "BotSocial.IgnoreGameMasters", true);

    _cfg.authDatabase = sConfigMgr->GetOption<std::string>(
        "BotSocial.AuthDatabaseName", "acore_auth");
    _cfg.botAccountPrefix = sConfigMgr->GetOption<std::string>(
        "BotSocial.BotAccountPrefix", "RNDBOT");
}

bool IsBot(Player* player)
{
    if (!player)
        return false;

    WorldSession* session = player->GetSession();
    return session && session->IsBot();
}

// ---- positive events ---------------------------------------------------

void OnLogin(Player* player)
{
    if (!_cfg.enable || !player)
        return;

    uint32 low = player->GetGUID().GetCounter();

    if (!IsBot(player))
    {
        // With Player.Enable off you are invisible to the whole system:
        // never remembered, never resented, never befriended.
        if (!_cfg.playerEnable)
            return;

        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        _humans.insert(low);
        return;
    }

    EnsureProfile(low);
}

void OnGroupMemberAdded(Group* group, ObjectGuid guid)
{
    if (!_cfg.enable || !group)
        return;

    uint32 joiner  = guid.GetCounter();
    uint32 groupId = group->GetGUID().GetCounter();

    {
        std::lock_guard<std::mutex> guard(_groupMutex);
        _groupSince[groupId][joiner] = time(nullptr);
    }

    std::string joinerType = ArchetypeOf(joiner);

    for (uint32 other : GroupMemberLowGuids(group))
    {
        if (other == joiner)
            continue;

        // Both sides gain from simply being here, but the newcomer
        // gains slightly less: they are the one being let in.
        Touch mine;
        mine.affinity = _cfg.gainGroupJoin;
        mine.grouped  = 1;
        mine.kind     = "group_join";

        Touch theirs = mine;
        theirs.affinity = _cfg.gainGroupJoin + 1;

        // Some people just do not get along.
        if (_cfg.clashPenalty > 0
            && Clashes(joinerType, ArchetypeOf(other)))
        {
            mine.affinity   -= _cfg.clashPenalty;
            theirs.affinity -= _cfg.clashPenalty;
        }

        Apply(joiner, other, mine);
        Apply(other, joiner, theirs);
    }
}

void OnGroupMemberRemoved(Group* group, ObjectGuid guid,
                          bool kicked, bool lfgKick, bool left,
                          ObjectGuid kicker)
{
    if (!_cfg.enable || !group)
        return;

    uint32 leaver  = guid.GetCounter();
    uint32 groupId = group->GetGUID().GetCounter();

    time_t since = 0;
    {
        std::lock_guard<std::mutex> guard(_groupMutex);
        auto git = _groupSince.find(groupId);
        if (git != _groupSince.end())
        {
            auto pit = git->second.find(leaver);
            if (pit != git->second.end())
            {
                since = pit->second;
                git->second.erase(pit);
            }
        }
    }

    // ---- the pleasant part: time spent together ----
    if (since)
    {
        int32 minutes = int32((time(nullptr) - since) / 60);
        if (minutes > 0)
        {
            minutes = std::min(minutes, _cfg.maxMinutesPerStint);

            Touch t;
            t.affinity = minutes * _cfg.gainPerMinute;
            t.minutes  = uint32(minutes);
            t.countsAsMeeting = false;
            t.kind     = "minutes";

            for (uint32 other : GroupMemberLowGuids(group))
                if (other != leaver)
                {
                    Apply(leaver, other, t);
                    Apply(other, leaver, t);
                }
        }
    }

    if (!_cfg.conflictEnable)
        return;

    // ---- the unpleasant part ----
    if (kicked)
    {
        uint32 by = kicker.GetCounter();
        int32 amount = lfgKick ? _cfg.lossKickedLfg : _cfg.lossKicked;

        Touch t;
        t.affinity   = -amount;
        t.grudges    = 1;
        t.grudgeKind = lfgKick ? "lfg_kick" : "kick";
        t.countsAsMeeting = false;

        if (by)
        {
            // The grudge is against the person who did it, not the
            // group. That is how being thrown out actually feels.
            Apply(leaver, by, t);
            LogEvent(by, leaver, t.grudgeKind, "", -amount);
        }
        else
        {
            for (uint32 other : GroupMemberLowGuids(group))
                if (other != leaver)
                    Apply(leaver, other, t);
        }

        if (_cfg.debug)
            LOG_INFO("module", "BotSocial: {} wurde von {} geworfen ({})",
                     leaver, by, t.grudgeKind);
        return;
    }

    // Walking out while the group is inside an instance is the classic
    // way to make five people dislike you at once. A group that simply
    // disbands after the run is not that, so only a deliberate leave
    // counts.
    if (!left)
        return;

    Player* leaverPlayer = ObjectAccessor::FindPlayer(guid);
    if (leaverPlayer && leaverPlayer->GetMap()
        && leaverPlayer->GetMap()->IsDungeon())
    {
        GroupGrudge(group, leaver, _cfg.lossBailed, "bailed",
                    "Instanz verlassen");
    }
}

void OnGroupDisbanded(Group* group)
{
    if (!group)
        return;

    // A disband settles the time everyone spent together and nothing
    // more - nobody walked out on anybody.
    for (auto const& slot : group->GetMemberSlots())
        OnGroupMemberRemoved(group, slot.guid, false, false, false,
                             ObjectGuid::Empty);

    std::lock_guard<std::mutex> guard(_groupMutex);
    _groupSince.erase(group->GetGUID().GetCounter());
}

void OnDungeonEntered(Player* player, uint32 /*mapId*/)
{
    if (!_cfg.enable || !player || IgnoredActor(player))
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    Touch t;
    t.affinity = _cfg.gainDungeon;
    t.dungeons = 1;
    t.countsAsMeeting = false;
    t.kind     = "dungeon";
    t.mapId    = player->GetMapId();
    t.zoneId   = player->GetZoneId();

    uint32 me = player->GetGUID().GetCounter();
    for (uint32 other : GroupMemberLowGuids(group))
        if (other != me)
            Apply(me, other, t);
}

void OnBattlegroundEntered(Player* player)
{
    if (!_cfg.enable || !player || IgnoredActor(player))
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    Touch t;
    t.affinity = _cfg.gainBattleground;
    t.battles  = 1;
    t.countsAsMeeting = false;
    t.kind     = "battleground";
    t.mapId    = player->GetMapId();
    t.zoneId   = player->GetZoneId();

    uint32 me = player->GetGUID().GetCounter();
    for (uint32 other : GroupMemberLowGuids(group))
        if (other != me)
            Apply(me, other, t);
}

void OnResurrected(Player* player)
{
    if (!_cfg.enable || !player || IgnoredActor(player))
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    // We cannot see who cast it, so the group as a whole gets the
    // credit. Being helped is worth more than helping - that is the
    // asymmetry that makes debts real.
    uint32 me = player->GetGUID().GetCounter();

    Touch grateful;
    grateful.affinity = _cfg.gainResurrect;
    grateful.favours  = 1;
    grateful.countsAsMeeting = false;
    grateful.kind     = "resurrected";
    grateful.mapId    = player->GetMapId();
    grateful.zoneId   = player->GetZoneId();

    Touch helper;
    helper.affinity = _cfg.gainResurrector;
    helper.countsAsMeeting = false;
    helper.kind     = "resurrector";
    helper.mapId    = grateful.mapId;
    helper.zoneId   = grateful.zoneId;

    for (uint32 other : GroupMemberLowGuids(group))
    {
        if (other == me)
            continue;
        Apply(me, other, grateful);
        Apply(other, me, helper);
        BumpReputation(other, _cfg.reputationPerFavour);
    }
}

// Reines Protokoll, keine Punkte: weight traegt das neue Level, damit
// sich Level-Kurven direkt aus bot_social_event lesen lassen. Haengt
// am Trace-Schalter, weil es dieselbe Tabelle fuellt und dieselbe
// Aufraeumregel (RetentionDays) braucht.
void OnLevelUp(Player* player, uint8 oldLevel)
{
    if (!_cfg.enable || !_cfg.traceEnable || !player
        || IgnoredActor(player))
        return;

    LogEvent(player->GetGUID().GetCounter(), 0, "level_up",
             std::to_string(uint32(oldLevel)),
             int32(player->GetLevel()),
             player->GetMapId(), player->GetZoneId());
}

// ---- conflict -----------------------------------------------------------

void OnLootRoll(Player* player, Item* item, uint32 voteType)
{
    if (!_cfg.enable || !_cfg.conflictEnable || !player
        || IgnoredActor(player))
        return;

    // NEED is 1. Only a need roll can be resented; greed and pass are
    // nobody's business.
    if (voteType != 1)
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    std::string detail = item ? item->GetTemplate()->Name1 : "";

    GroupGrudge(group, player->GetGUID().GetCounter(),
                _cfg.lossNinjaLoot, "ninja", detail,
                player->GetMapId(), player->GetZoneId());
}

void OnGuildMemberRemoved(Guild* guild, Player* player, bool kicked)
{
    if (!_cfg.enable || !_cfg.conflictEnable || !guild || !player)
        return;
    if (!kicked)
        return;

    uint32 master = guild->GetLeaderGUID().GetCounter();
    uint32 low    = player->GetGUID().GetCounter();

    Touch t;
    t.affinity   = -_cfg.lossGuildKick;
    t.grudges    = 1;
    t.grudgeKind = "guild_kick";
    t.countsAsMeeting = false;

    Apply(low, master, t);
    LogEvent(master, low, "guild_kick", guild->GetName(),
             -_cfg.lossGuildKick,
             player->GetMapId(), player->GetZoneId());

    if (_cfg.debug)
        LOG_INFO("module", "BotSocial: {} wurde aus '{}' geworfen",
                 player->GetName(), guild->GetName());
}

void OnGuildMoneyWithdrawn(Guild* guild, Player* player, uint32 amount)
{
    if (!_cfg.enable || !_cfg.conflictEnable || !guild || !player
        || IgnoredActor(player))
        return;
    if (amount < _cfg.bankDrainCopper)
        return;

    uint32 low = player->GetGUID().GetCounter();

    Touch t;
    t.affinity   = -_cfg.lossBankDrain;
    t.grudges    = 1;
    t.grudgeKind = "bank_drain";
    t.countsAsMeeting = false;

    for (uint32 m : GuildMemberGuids(guild->GetId()))
        if (m != low)
            Apply(m, low, t);

    BumpReputation(low, -_cfg.reputationPerGrudge);
    LogEvent(low, 0, "bank_drain", guild->GetName(), int32(amount),
             player->GetMapId(), player->GetZoneId());
}

void OnPvpKill(Player* killer, Player* killed)
{
    if (!_cfg.enable || !_cfg.conflictEnable || !killer || !killed
        || IgnoredActor(killer))
        return;

    // Only a lopsided kill counts. A fair fight is not a grievance.
    if (uint32(killer->GetLevel())
        < uint32(killed->GetLevel()) + _cfg.gankLevelGap)
        return;

    Touch t;
    t.affinity   = -_cfg.lossGank;
    t.grudges    = 1;
    t.grudgeKind = "gank";
    t.countsAsMeeting = false;

    Apply(killed->GetGUID().GetCounter(),
          killer->GetGUID().GetCounter(), t);

    LogEvent(killer->GetGUID().GetCounter(),
             killed->GetGUID().GetCounter(),
             "gank", killed->GetName(), -_cfg.lossGank,
             killed->GetMapId(), killed->GetZoneId());
}

void OnDuelFinished(Player* winner, Player* loser)
{
    if (!_cfg.enable || !_cfg.conflictEnable || !winner || !loser
        || IgnoredActor(winner))
        return;

    Touch t;
    t.affinity = -_cfg.lossDuel;
    t.countsAsMeeting = false;

    Apply(loser->GetGUID().GetCounter(),
          winner->GetGUID().GetCounter(), t);
}

// ---- decisions ----------------------------------------------------------

bool WouldRefuseGroup(Player* player, Group* group)
{
    if (!_cfg.enable || !_cfg.avoidEnable || !player || !group)
        return false;

    uint32 me = player->GetGUID().GetCounter();

    for (uint32 other : GroupMemberLowGuids(group))
    {
        if (other == me)
            continue;

        // Never refuse because of the human. You are always welcome.
        if (IsHuman(other) && !_cfg.playerCanBeIgnored)
            continue;

        if (AffinityOf(me, other) <= _cfg.avoidThreshold)
            return true;
    }

    return false;
}

bool WouldRefuseInvite(Player* player, std::string const& targetName)
{
    if (!_cfg.enable || !_cfg.avoidEnable || !player || targetName.empty())
        return false;

    uint32 target = 0;
    if (!ResolveCharacter(targetName, target))
        return false;

    if (IsHuman(target) && !_cfg.playerCanBeIgnored)
        return false;

    return AffinityOf(player->GetGUID().GetCounter(), target)
           <= _cfg.avoidThreshold;
}

// ---- lifecycle ----------------------------------------------------------

void Startup()
{
    if (!_cfg.enable)
    {
        LOG_INFO("module",
                 "mod-bot-social ist deaktiviert (BotSocial.Enable = 0)");
        return;
    }

    LoadGraph();

    // The trace log grows with every award; old rows carry no decision,
    // they are pure observation. Prune them once per server start.
    if (_cfg.traceRetentionDays > 0)
        CharacterDatabase.Execute(
            "DELETE FROM bot_social_event "
            "WHERE created_at < NOW() - INTERVAL {} DAY",
            _cfg.traceRetentionDays);

    CharacterDatabase.Execute(
        "INSERT IGNORE INTO bot_social_guild (guild_id, target_size) "
        "SELECT g.guildid, {} FROM guild g "
        "JOIN characters c ON c.guid = g.leaderguid "
        "JOIN {}.account a ON a.id = c.account "
        "WHERE a.username LIKE '{}%'",
        _cfg.guildTargetMin, _cfg.authDatabase, _cfg.botAccountPrefix);

    QueryResult res = CharacterDatabase.Query(
        "SELECT guild_id FROM bot_social_guild WHERE target_size = {}",
        _cfg.guildTargetMin);

    if (res)
    {
        do
        {
            CharacterDatabase.Execute(
                "UPDATE bot_social_guild SET target_size = {} "
                "WHERE guild_id = {}",
                RollBetween(_cfg.guildTargetMin, _cfg.guildTargetMax),
                res->Fetch()[0].Get<uint32>());
        } while (res->NextRow());
    }

    // Rows seeded before the cap existed (or before it was lowered) still carry oversized targets.
    // RecruitTick reads target_size per row, not the config, so those guilds would keep recruiting
    // past the ceiling until re-rolled. Bring them down in place.
    if (_cfg.guildCapacityCap)
        CharacterDatabase.Execute(
            "UPDATE bot_social_guild SET target_size = {} WHERE target_size > {}",
            _cfg.guildTargetMax, _cfg.guildTargetMax);

    LOG_INFO("module",
             "mod-bot-social aktiv: Streit={} Freunde={} Werbung={} "
             "Spaltung={}",
             _cfg.conflictEnable, _cfg.friendsEnable,
             _cfg.recruitEnable, _cfg.schismEnable);
}

void Shutdown()
{
    FlushGraph();
}

void Update(uint32 diff)
{
    if (!_cfg.enable)
        return;

    _flushTimer += diff;
    if (_flushTimer >= _cfg.flushInterval * IN_MILLISECONDS)
    {
        _flushTimer = 0;
        FlushGraph();
        ApplySocialLists();
    }

    _recruitTimer += diff;
    if (_recruitTimer >= _cfg.recruitInterval * IN_MILLISECONDS)
    {
        _recruitTimer = 0;
        RecruitTick();
    }

    _rankTimer += diff;
    if (_rankTimer >= _cfg.rankPromoteInterval * IN_MILLISECONDS)
    {
        _rankTimer = 0;
        RankTick();
    }

    _schismTimer += diff;
    if (_schismTimer >= _cfg.schismInterval * IN_MILLISECONDS)
    {
        _schismTimer = 0;
        SchismTick();
    }

    _decayTimer += diff;
    if (uint64(_decayTimer)
        >= uint64(_cfg.decayIntervalHours) * 3600u * IN_MILLISECONDS)
    {
        _decayTimer = 0;
        DecayTick();
        EnforceDunbar();
    }
}

// ---- queries -------------------------------------------------------------

namespace
{
    std::vector<BondRow> CollectBonds(uint32 botGuid, uint32 limit,
                                      bool worst)
    {
        std::vector<BondRow> out;

        std::vector<std::pair<uint32, Edge>> edges;
        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);
            auto it = _graph.find(botGuid);
            if (it == _graph.end())
                return out;

            for (auto const& e : it->second)
                edges.emplace_back(e.first, e.second);
        }

        std::sort(edges.begin(), edges.end(),
                  [worst](std::pair<uint32, Edge> const& a,
                          std::pair<uint32, Edge> const& b)
                  {
                      return worst ? a.second.affinity < b.second.affinity
                                   : a.second.affinity > b.second.affinity;
                  });

        if (edges.size() > limit)
            edges.resize(limit);

        // One query for every name, not one per row.
        std::ostringstream guids;
        bool firstGuid = true;
        for (auto const& e : edges)
        {
            if (!firstGuid)
                guids << ",";
            guids << e.first;
            firstGuid = false;
        }

        std::unordered_map<uint32, std::string> names;
        if (!firstGuid)
        {
            QueryResult nres = CharacterDatabase.Query(
                "SELECT guid, name FROM characters WHERE guid IN ({})",
                guids.str());

            if (nres)
            {
                do
                {
                    Field* nf = nres->Fetch();
                    names[nf[0].Get<uint32>()] = nf[1].Get<std::string>();
                } while (nres->NextRow());
            }
        }

        for (auto const& e : edges)
        {
            BondRow row;
            row.otherGuid       = e.first;
            row.affinity        = e.second.affinity;
            row.timesGrouped    = e.second.grouped;
            row.minutesTogether = e.second.minutes;
            row.dungeons        = e.second.dungeons;
            row.grudges         = e.second.grudges;
            row.lastGrudge      = e.second.lastGrudge;
            row.befriended      = e.second.befriended;
            row.ignored         = e.second.ignored;

            auto nit = names.find(e.first);
            row.otherName = nit == names.end() ? "?" : nit->second;

            out.push_back(row);
        }

        return out;
    }
}

std::vector<BondRow> TopBonds(uint32 botGuid, uint32 limit)
{
    return CollectBonds(botGuid, limit, false);
}

std::vector<BondRow> WorstBonds(uint32 botGuid, uint32 limit)
{
    return CollectBonds(botGuid, limit, true);
}

ProfileRow GetProfile(uint32 botGuid)
{
    ProfileRow row;

    std::lock_guard<std::recursive_mutex> guard(_graphMutex);
    auto it = _profiles.find(botGuid);
    if (it == _profiles.end())
        return row;

    row.archetype   = it->second.archetype;
    row.skillTier   = it->second.skillTier;
    row.sociability = it->second.sociability;
    row.ambition    = it->second.ambition;
    row.reputation  = it->second.reputation;
    row.found       = true;
    return row;
}

bool ResolveCharacter(std::string const& name, uint32& guidOut)
{
    if (name.empty())
        return false;

    QueryResult res = CharacterDatabase.Query(
        "SELECT guid FROM characters WHERE name = '{}' LIMIT 1",
        Escape(name));

    if (!res)
        return false;

    guidOut = res->Fetch()[0].Get<uint32>();
    return true;
}

Stats Snapshot()
{
    Stats s;

    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        for (auto const& node : _graph)
        {
            for (auto const& e : node.second)
            {
                ++s.bondRows;
                if (e.second.befriended) ++s.friendships;
                if (e.second.affinity < 0) ++s.grudges;
                if (e.second.ignored) ++s.ignores;
                if (e.second.dirty) ++s.pendingWrites;
            }
        }
    }

    if (QueryResult res = CharacterDatabase.Query(
            "SELECT COUNT(*), COALESCE(SUM(recruited), 0) "
            "FROM bot_social_guild"))
    {
        Field* f = res->Fetch();
        s.guildsTracked = f[0].Get<uint64>();
        s.recruited     = f[1].Get<uint64>();
    }

    s.schisms = _schismTotal;
    return s;
}

} // namespace BotSocial
