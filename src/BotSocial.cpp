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
#include "SharedDefines.h"   // TeamId / TEAM_ALLIANCE fuer die Gruendung
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <shared_mutex>
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
        // Wie viel des aktuellen Grolls nur gehoert und nicht erlebt
        // ist. Immer <= -affinity. Verblasst schneller (siehe DecayTick).
        int32  hearsay    = 0;
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
        // Schicht 1: Taetigkeit
        std::string archetype = "unset";
        // Koennen: eigene Groesse
        uint32 skillTier   = 2;
        // Schicht 2: Wesen - unabhaengig vom Archetyp gewuerfelt
        uint32 sociability = 50;
        uint32 ambition    = 50;
        uint32 agreeableness     = 50;
        uint32 honesty           = 50;
        uint32 conscientiousness = 50;
        uint32 sadism            = 0;
        // Schicht 3: Beweggrund - abgeleitet aus 1, 5 und Gilde
        std::string gearMotive = "means";
        // Schicht 4: Verfuegbarkeit - die fuenf Teile gehoeren zusammen
        uint32 playMinutesWeek   = 1360;
        uint32 primeHour         = 18;
        uint32 primeSpan         = 2;
        uint32 blockMinutes      = 60;
        uint32 commitReliability = 50;
        uint32 interruptibility  = 50;
        // Schicht 5: Verlauf
        std::string stage = "entry";
        uint32 stageSince = 0;
        // Schicht 6: Anker - wird verdient, nie gewuerfelt
        uint32 anchorGuid = 0;

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
    uint32 _foundTimer   = 0;
    time_t _lastFound    = 0;
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

    // Normalverteilt, hart gekappt. Fuer die Wesenszuege: die Masse sitzt
    // in der Mitte, die Raender sind duenn - anders als bei RollBetween,
    // wo jeder Wert gleich haeufig ist.
    uint32 RollNormal(int32 mean, int32 spread, uint32 lo, uint32 hi)
    {
        if (spread <= 0)
            return uint32(mean < int32(lo) ? lo
                        : mean > int32(hi) ? hi : uint32(mean));

        // Nicht dist(double(mean), double(spread)) schreiben: das wird als
        // Funktionsdeklaration geparst (most vexing parse) und kompiliert
        // nicht. Erst benennen, dann uebergeben.
        double const m = double(mean);
        double const sd = double(spread);
        std::normal_distribution<double> dist(m, sd);
        double v = dist(Rng());
        if (v < double(lo)) v = double(lo);
        if (v > double(hi)) v = double(hi);
        return uint32(v + 0.5);
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
        // Geruechte duerfen den Ersteindruck-Multiplikator nicht
        // ausloesen: ein Fremder, von dem man nur gehoert hat, wuerde
        // sonst doppelt zaehlen.
        bool   ignoreFirstImpression = false;
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
    // Definiert unterhalb von Apply, weil es dort gelesen wird, wo es
    // hingehoert - der Aufruf steht am Ende von Apply.
    void MaybeAnchor(uint32 from, uint32 to, Edge const& e,
                     Touch const& t, int32 weight);

    // Die Zugriffe auf die Wesenszuege stehen weiter unten bei den
    // uebrigen Profilhelfern, werden aber schon hier gebraucht: die
    // Vertraeglichkeit skaliert den Groll in Apply und steuert den
    // Grollzerfall in DecayTick.
    int32  AgreeablenessOf(uint32 guid);
    uint32 InterruptibilityOf(uint32 guid);
    uint32 ConscientiousnessOf(uint32 guid);

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
        if (!t.ignoreFirstImpression
            && e.encounters < _cfg.firstImpressions)
            weight *= _cfg.firstImpressionMul;

        // Wie schwer jemand etwas nimmt, haengt an ihm, nicht an der Tat.
        // 'from' ist der, der den Groll traegt. Nur Negatives wird
        // skaliert: Freundlichkeit macht dankbarer zu behaupten, waere
        // eine zweite Behauptung ohne Beleg.
        if (_cfg.traitsAffect && weight < 0)
        {
            int32 agr = AgreeablenessOf(from);
            int32 factor = 100 + (50 - agr) * _cfg.traitGrudgeSwing / 100;
            if (factor < 20)  factor = 20;
            if (factor > 200) factor = 200;

            weight = weight * factor / 100;
            if (!weight)
                weight = -1;   // ganz verzeihen tut auch der Sanfteste nicht
        }

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

        MaybeAnchor(from, to, e, t, weight);
    }

    // Schicht 6. Ein Anker wird verdient, nie gewuerfelt: aus gemeinsam
    // Durchgestandenem, nicht aus der Zuneigungszahl. Die ist breit und
    // verfaellt; ein Anker soll selten sein und bleiben.
    //
    // Deshalb haengt er am Dungeon-Ereignis und nicht an einem Zeitgeber:
    // er entsteht in dem Moment, in dem die beiden etwas zusammen
    // durchgestanden haben.
    void MaybeAnchor(uint32 from, uint32 to, Edge const& e,
                     Touch const& t, int32 weight)
    {
        if (!_cfg.anchorEnable)
            return;

        auto it = _profiles.find(from);
        if (it == _profiles.end())
            return;   // Menschen und Unbekannte bekommen keinen Anker

        Profile& p = it->second;

        // Reissen: ein schwerer Schlag von genau dem, an dem man haengt.
        if (p.anchorGuid == to && weight <= -_cfg.anchorBreakAt)
        {
            p.anchorGuid = 0;
            p.dirty = true;
            LogEvent(from, to, "anchor_broken", "", weight,
                     t.mapId, t.zoneId);
            return;
        }

        if (p.anchorGuid || !t.dungeons)
            return;   // wer einen hat, bekommt keinen zweiten

        if (e.affinity <= 0
            || e.dungeons < _cfg.anchorMinDungeons
            || e.minutes  < _cfg.anchorMinMinutes)
            return;

        p.anchorGuid = to;
        p.dirty = true;
        LogEvent(from, to, "anchor_formed", "", e.affinity,
                 t.mapId, t.zoneId);
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

    // ---- Geruechte ------------------------------------------------------

    // Das Opfer erzaehlt seinen Freunden, was passiert ist. Wie viel
    // haengenbleibt, entscheidet das Vertrauen des Zuhoerers zum
    // Erzaehler - nicht, ob es stimmt. Ein beliebter Uebeltaeter
    // uebersteht das, ein unbeliebtes Opfer wird nicht ernst genommen.
    //
    // severity ist der Betrag des erlebten Grolls (positiv).
    void SpreadRumour(uint32 teller, uint32 offender, int32 severity,
                      std::string const& kind, uint32 mapId, uint32 zoneId)
    {
        if (!_cfg.gossipEnable || !teller || !offender || teller == offender)
            return;
        if (severity < _cfg.gossipMinSeverity)
            return;

        // Ueber den Spieler wird nicht getratscht, solange er das nicht
        // erlaubt: von hunderten Bots ausgefroren zu werden waere kein
        // Realismus, sondern ein kaputtes Spiel.
        if (IsHuman(offender) && !_cfg.gossipAboutPlayers)
            return;

        // Erst sammeln, dann anwenden. Apply() legt neue Knoten in
        // _graph an; wuerde man dabei ueber _graph iterieren, koennte
        // das Rehashing die Referenzen unter den Fuessen wegziehen.
        std::vector<std::pair<uint32, int32>> listeners;
        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);

            auto it = _graph.find(teller);
            if (it == _graph.end())
                return;

            for (auto const& edge : it->second)
            {
                uint32 listener = edge.first;
                if (listener == offender || listener == teller)
                    continue;
                if (edge.second.affinity < _cfg.gossipMinTrust)
                    continue;

                listeners.push_back(
                    std::make_pair(listener, edge.second.affinity));
            }
        }

        if (listeners.empty())
            return;

        // Die engsten Freunde erfahren es zuerst.
        std::sort(listeners.begin(), listeners.end(),
                  [](std::pair<uint32, int32> const& a,
                     std::pair<uint32, int32> const& b)
                  { return a.second > b.second; });

        uint32 told     = 0;
        uint32 strangers = 0;

        for (auto const& l : listeners)
        {
            if (told >= _cfg.gossipMaxListeners)
                break;

            uint32 listener = l.first;
            int32  trust    = l.second;

            // Loyalitaet schlaegt Hoerensagen: wer den Beschuldigten
            // lieber mag als den Erzaehler, glaubt die Geschichte nicht.
            if (AffinityOf(listener, offender) > trust)
                continue;

            bool isStranger;
            {
                std::lock_guard<std::recursive_mutex> guard(_graphMutex);
                auto it = _graph.find(listener);
                isStranger = it == _graph.end()
                          || it->second.find(offender) == it->second.end();
            }

            // Neue Bindungen sind der teure Teil: sie lassen die Tabelle
            // wachsen, wovor die Dunbar-Grenze eigentlich schuetzt.
            if (isStranger)
            {
                if (strangers >= _cfg.gossipMaxStrangers)
                    continue;
                ++strangers;
            }

            int32 share = severity * _cfg.gossipPercent / 100;
            if (_cfg.gossipFullTrust > 0 && trust < _cfg.gossipFullTrust)
                share = share * trust / _cfg.gossipFullTrust;

            if (share <= 0)
                continue;

            Touch t;
            t.affinity   = -share;
            t.grudgeKind = "hearsay";
            t.countsAsMeeting       = false;
            t.ignoreFirstImpression = true;
            // kind bleibt leer: Apply() soll nicht selbst protokollieren,
            // wir schreiben die Zeile unten mit dem Erzaehler im Detail.

            Apply(listener, offender, t);

            {
                std::lock_guard<std::recursive_mutex> guard(_graphMutex);
                Edge& e = _graph[listener][offender];
                e.hearsay += share;
                if (e.hearsay > -e.affinity)
                    e.hearsay = std::max(0, -e.affinity);
            }

            if (_cfg.traceEnable && share >= _cfg.traceMinWeight)
                LogEvent(listener, offender, "gossip",
                         std::to_string(teller), -share, mapId, zoneId);

            ++told;
        }

        if (told && _cfg.debug)
            LOG_INFO("module",
                     "BotSocial: {} erzaehlte {} Leuten von {} ({})",
                     teller, told, offender, kind);
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
            // ... und erzaehlt es hinterher seinen Freunden.
            SpreadRumour(other, culprit, amount, kind, mapId, zoneId);
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
            "befriended, ignored, last_grudge, hearsay "
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
                e.hearsay    = f[13].Get<int32>();
                e.dirty      = false;
                ++loaded;
            } while (res->NextRow());
        }

        QueryResult pres = CharacterDatabase.Query(
            "SELECT bot_guid, archetype, skill_tier, sociability, "
            "ambition, reputation, agreeableness, honesty, "
            "conscientiousness, sadism, gear_motive, play_minutes_week, "
            "prime_hour, prime_span, block_minutes, commit_reliability, "
            "interruptibility, stage, stage_since, anchor_guid "
            "FROM bot_social_profile");

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
                p.agreeableness     = f[6].Get<uint8>();
                p.honesty           = f[7].Get<uint8>();
                p.conscientiousness = f[8].Get<uint8>();
                p.sadism            = f[9].Get<uint8>();
                p.gearMotive        = f[10].Get<std::string>();
                p.playMinutesWeek   = f[11].Get<uint16>();
                p.primeHour         = f[12].Get<uint8>();
                p.primeSpan         = f[13].Get<uint8>();
                p.blockMinutes      = f[14].Get<uint16>();
                p.commitReliability = f[15].Get<uint8>();
                p.interruptibility  = f[16].Get<uint8>();
                p.stage             = f[17].Get<std::string>();
                p.stageSince        = f[18].Get<uint32>();
                p.anchorGuid        = f[19].Get<uint32>();
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

        // Wer hier nicht drankommt, bleibt schmutzig und geht in der
        // naechsten Runde raus. Nichts geht verloren, es dauert nur.
        uint32 const budget = _cfg.flushMaxStatements
                            ? _cfg.flushMaxStatements : 0xFFFFFFFFu;
        bool full = false;

        {
            std::lock_guard<std::recursive_mutex> guard(_graphMutex);

            for (auto& node : _graph)
            {
                if (full)
                    break;

                for (auto& edge : node.second)
                {
                    if (statements.size() >= budget)
                    {
                        full = true;
                        break;
                    }

                    Edge& e = edge.second;
                    if (!e.dirty)
                        continue;

                    std::ostringstream sql;
                    sql << "INSERT INTO bot_social_bond "
                           "(bot_guid, other_guid, affinity, encounters, "
                           "times_grouped, minutes_together, "
                           "dungeons_together, battles_together, "
                           "grudge_events, favours_owed, befriended, "
                           "ignored, last_grudge, hearsay) VALUES ("
                        << node.first << ", " << edge.first << ", "
                        << e.affinity << ", " << e.encounters << ", "
                        << e.grouped << ", " << e.minutes << ", "
                        << e.dungeons << ", " << e.battles << ", "
                        << e.grudges << ", " << e.favours << ", "
                        << (e.befriended ? 1 : 0) << ", "
                        << (e.ignored ? 1 : 0) << ", '"
                        << Escape(e.lastGrudge) << "', "
                        << e.hearsay
                        << ") ON DUPLICATE KEY UPDATE "
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
                        << "', hearsay = " << e.hearsay
                        << ", last_seen = NOW()";

                    statements.push_back(sql.str());
                    e.dirty = false;
                }
            }

            for (auto& entry : _profiles)
            {
                if (statements.size() >= budget)
                    break;

                Profile& p = entry.second;
                if (!p.dirty)
                    continue;

                std::ostringstream sql;
                sql << "INSERT INTO bot_social_profile "
                       "(bot_guid, archetype, skill_tier, sociability, "
                       "ambition, reputation, agreeableness, honesty, "
                       "conscientiousness, sadism, gear_motive, "
                       "play_minutes_week, prime_hour, prime_span, "
                       "block_minutes, commit_reliability, interruptibility, "
                       "stage, stage_since, anchor_guid) VALUES ("
                    << entry.first << ", '" << Escape(p.archetype) << "', "
                    << p.skillTier << ", " << p.sociability << ", "
                    << p.ambition << ", " << p.reputation << ", "
                    << p.agreeableness << ", " << p.honesty << ", "
                    << p.conscientiousness << ", " << p.sadism << ", '"
                    << Escape(p.gearMotive) << "', "
                    << p.playMinutesWeek << ", " << p.primeHour << ", "
                    << p.primeSpan << ", " << p.blockMinutes << ", "
                    << p.commitReliability << ", " << p.interruptibility
                    << ", '" << Escape(p.stage) << "', " << p.stageSince
                    << ", " << p.anchorGuid
                    << ") ON DUPLICATE KEY UPDATE "
                       "archetype = '" << Escape(p.archetype)
                    << "', skill_tier = " << p.skillTier
                    << ", sociability = " << p.sociability
                    << ", ambition = " << p.ambition
                    << ", reputation = " << p.reputation
                    << ", agreeableness = " << p.agreeableness
                    << ", honesty = " << p.honesty
                    << ", conscientiousness = " << p.conscientiousness
                    << ", sadism = " << p.sadism
                    << ", gear_motive = '" << Escape(p.gearMotive)
                    << "', play_minutes_week = " << p.playMinutesWeek
                    << ", prime_hour = " << p.primeHour
                    << ", prime_span = " << p.primeSpan
                    << ", block_minutes = " << p.blockMinutes
                    << ", commit_reliability = " << p.commitReliability
                    << ", interruptibility = " << p.interruptibility
                    << ", stage = '" << Escape(p.stage)
                    << "', stage_since = " << p.stageSince
                    << ", anchor_guid = " << p.anchorGuid;

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
                    e.hearsay = 0;
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

                    // Wer nachtraegt, traegt laenger nach. node.first ist
                    // der Traeger des Grolls, nicht sein Ziel.
                    if (_cfg.traitsAffect && !aboutHuman)
                    {
                        int32 agr = AgreeablenessOf(node.first);

                        if (agr >= int32(_cfg.traitForgiveFrom))
                            tick = true;
                        else if (agr <= int32(_cfg.traitGrudgeHoldBelow)
                                 && _cfg.decayNegativeDivisor)
                            tick = (_decayRound
                                    % (_cfg.decayNegativeDivisor * 2)) == 0;
                    }

                    if (!tick || _cfg.decayNegative <= 0)
                        continue;

                    int32 step = _cfg.decayNegative;
                    if (aboutHuman)
                        step *= int32(_cfg.playerGrudgeSpeedup);

                    e.affinity = std::min(0, e.affinity + step);
                    e.dirty = true;

                    // Was man nur gehoert hat, verblasst schneller als
                    // Erlebtes. Der zusaetzliche Schritt zehrt nur am
                    // Hoerensagen-Anteil und nie darueber hinaus.
                    if (e.hearsay > 0)
                    {
                        int32 extra = 0;
                        if (_cfg.gossipDecayFactor > 1)
                            extra = _cfg.decayNegative
                                  * int32(_cfg.gossipDecayFactor - 1);

                        if (extra > e.hearsay)
                            extra = e.hearsay;

                        if (extra > 0)
                        {
                            e.affinity = std::min(0, e.affinity + extra);
                            e.hearsay -= extra;
                        }

                        // Der erlebte Schritt oben hat den Groll ebenfalls
                        // verkleinert - die Invariante nachziehen.
                        if (e.hearsay > -e.affinity)
                            e.hearsay = std::max(0, -e.affinity);
                    }

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

    // Was jemand TUT, nicht wie er ist. Frueher standen hier auch
    // Geselligkeit und Ehrgeiz - damit folgte die Persoenlichkeit der
    // Taetigkeit. Dafuer gibt es keinen empirischen Beleg, also ist das
    // Wesen jetzt Schicht 2 und wird unabhaengig gezogen.
    //
    // 'casual' und 'loner' sind entfallen: das eine war ein Zeitprofil,
    // das andere ein Wesenszug. Beide entstehen jetzt an ihrer Schicht.
    struct ArchetypeDef
    {
        char const* name;
        uint32 weight;
        // Neigung zu einem der vier Zeitprofile (0 = keine).
        uint32 timeBias;
    };

    enum TimeProfile : uint32
    {
        TIME_NONE      = 0,
        TIME_EVENING   = 1, // Feierabend, langer Block, verlaesslich
        TIME_ALONGSIDE = 2, // nebenher, kurz, jederzeit weg
        TIME_HEAVY     = 3, // viel Zeit, breites Fenster
        TIME_NIGHT     = 4, // nach 22 Uhr, laeuft ueber Mitternacht
    };

    ArchetypeDef const kArchetypes[] = {
        { "questor",    26, TIME_NONE      },
        { "dungeoneer", 18, TIME_NONE      },
        { "raider",     10, TIME_EVENING   },
        { "pvper",      12, TIME_NONE      },
        { "gatherer",   14, TIME_ALONGSIDE },
        { "explorer",   10, TIME_ALONGSIDE },
        { "collector",  10, TIME_NONE      },
    };

    // Welche Paarungen sich reiben. Progress gegen Gelegenheit steht hier
    // NICHT mehr: das war nie ein Taetigkeitskonflikt, sondern einer der
    // Zeitfenster. Er gehoert an Schicht 4 und kommt dort wieder.
    bool Clashes(std::string const& a, std::string const& b)
    {
        if (a == "raider" && b == "collector") return true;
        if (a == "collector" && b == "raider") return true;
        if (a == "raider" && b == "explorer")  return true;
        if (a == "explorer" && b == "raider")  return true;
        if (a == "pvper" && b == "gatherer")   return true;
        if (a == "gatherer" && b == "pvper")   return true;
        return false;
    }

    ArchetypeDef const* PickArchetype()
    {
        uint32 total = 0;
        for (ArchetypeDef const& a : kArchetypes)
            total += a.weight;

        uint32 roll = RollBetween(1, total);
        for (ArchetypeDef const& a : kArchetypes)
        {
            if (roll <= a.weight)
                return &a;
            roll -= a.weight;
        }
        return &kArchetypes[0];
    }

    // Sadismus ist nicht normalverteilt. Volkmer 2023 (vorregistriert,
    // N = 1026) zeigt ueber Quantilregression, dass Sadismus nur die
    // OBEREN Quantile der Trollneigung erklaert - fuer niedrige Werte
    // sagt er nichts. Ein durchgehender Regler waere also falsch.
    //
    // Die erste Fassung zog dafuer aus ZWEI Toepfen: 95 % aus 0..QuietMax,
    // 5 % aus TailFloor..100. Das erfuellt die Vorgabe, reisst aber ein
    // Loch: zwischen 16 und 39 existierte in 3518 Profilen NIEMAND. Damit
    // ist der Wert in Wahrheit binaer - jede spaetere Schwelle im Loch
    // verhaelt sich wie eine bei TailFloor, es gibt keinen Verlauf.
    //
    // Der zweite Topf bleibt also, er bekommt nur einen VERLAUF statt
    // einer Untergrenze - und er beginnt direkt ueber QuietMax, nicht
    // erst bei TailFloor. Damit ist die Strecke dazwischen besetzt.
    //
    //   ruhiger Topf : Anteil 1 - 2p, gleichverteilt 0 .. QuietMax
    //   Randtopf     : Anteil 2p,     Y = lo + (100-lo) * u^m
    //                  mit lo = QuietMax + 1
    //
    // m wird so gewaehlt, dass GENAU die Haelfte des Randtopfes TailFloor
    // erreicht. Dann liegt der Anteil ueber TailFloor bei 2p * 1/2 = p,
    // also exakt bei TailPercent - die Einstellung behaelt ihre Bedeutung:
    //
    //     m = ln((TailFloor - lo) / (100 - lo)) / ln(1/2)
    //
    // Bei 5 % und 40 sind das rund 90 % ruhig, 5 % dazwischen, 5 % ab 40.
    // Ein reines Potenzgesetz ueber den ganzen Bereich waere kuerzer, legt
    // aber drei Viertel aller Bots auf die glatte Null - dieselbe Art von
    // Artefakt, nur an anderer Stelle.
    uint32 RollSadism()
    {
        uint32 const tailPct = _cfg.sadismTailPercent;
        uint32 const floorAt = _cfg.sadismTailLo;
        uint32 const quiet   = _cfg.sadismQuietMax;

        uint32 const lo = quiet + 1;

        // Entartete Einstellungen: kein Rand gewuenscht, oder kein Platz
        // mehr zwischen ruhigem Topf und Obergrenze.
        if (!tailPct || tailPct * 2 >= 100 || floorAt <= lo || floorAt >= 100)
            return RollBetween(0, quiet > 100 ? 100 : quiet);

        if (RollBetween(1, 100) > tailPct * 2)
            return RollBetween(0, quiet);

        double const m = std::log(double(floorAt - lo) / double(100 - lo)) /
                         std::log(0.5);

        // u aus (0,1] - die Null muss draussen bleiben.
        double const u = double(RollBetween(1, 10000)) / 10000.0;

        double const v = double(lo) + double(100 - lo) * std::pow(u, m);
        long   const r = std::lround(v);

        return uint32(r < 0 ? 0 : (r > 100 ? 100 : r));
    }

    // Die fuenf Bestandteile der Verfuegbarkeit gehoeren ZUSAMMEN gezogen.
    // Einzeln gewuerfelt entstuenden unmoegliche Menschen: drei Stunden am
    // Stueck und trotzdem jederzeit weg.
    void RollTimeProfile(Profile& p, uint32 bias)
    {
        uint32 wEve = _cfg.timeWeightEvening;
        uint32 wAlo = _cfg.timeWeightAlongside;
        uint32 wHea = _cfg.timeWeightHeavy;
        uint32 wNig = _cfg.timeWeightNight;

        // Die Taetigkeit schiebt, sie bestimmt nicht. Ein Raider ist
        // haeufiger, aber nicht zwingend Feierabendspieler.
        if (bias == TIME_EVENING)   wEve += wEve;
        if (bias == TIME_ALONGSIDE) wAlo += wAlo;
        if (bias == TIME_HEAVY)     wHea += wHea;
        if (bias == TIME_NIGHT)     wNig += wNig;

        uint32 total = wEve + wAlo + wHea + wNig;
        if (!total)
            total = 1;

        uint32 roll = RollBetween(1, total);
        uint32 kind = roll <= wEve               ? TIME_EVENING
                    : roll <= wEve + wAlo        ? TIME_ALONGSIDE
                    : roll <= wEve + wAlo + wHea ? TIME_HEAVY
                                                 : TIME_NIGHT;

        switch (kind)
        {
            case TIME_EVENING:
                p.primeHour         = RollBetween(19, 22);
                p.primeSpan         = RollBetween(3, 5);
                p.blockMinutes      = RollBetween(120, 240);
                p.commitReliability = RollBetween(65, 95);
                p.interruptibility  = RollBetween(5, 30);
                break;

            case TIME_ALONGSIDE:
                p.primeHour         = RollBetween(8, 21);
                p.primeSpan         = RollBetween(1, 3);
                // Obergrenze 75 statt 60: zwischen 60 und 90 Minuten lag
                // sonst dieselbe Art Loch wie beim Sadismus - niemand
                // spielte eine Stunde und ein Viertel am Stueck, obwohl
                // das die haeufigste Sitzung ueberhaupt ist.
                p.blockMinutes      = RollBetween(20, 75);
                p.commitReliability = RollBetween(15, 50);
                p.interruptibility  = RollBetween(60, 95);
                break;

            case TIME_HEAVY:
                p.primeHour         = RollBetween(12, 18);
                p.primeSpan         = RollBetween(5, 9);
                p.blockMinutes      = RollBetween(76, 200);
                p.commitReliability = RollBetween(45, 85);
                p.interruptibility  = RollBetween(20, 55);
                break;

            // Die Nachtschicht. Ohne sie endete der Serverabend um 22 Uhr
            // und zwischen 23 und 7 war die Welt leer - fuer ein Spiel,
            // dessen Feierabendgipfel real bis nach Mitternacht laeuft,
            // ist das die groebste Luecke der ganzen Schicht.
            //
            // Der Beginn laeuft ueber Mitternacht (22, 23, 0, 1, 2). Das
            // ist erlaubt: WindowOverlap und GuildWindow rechnen beide
            // modulo 24, ein Fenster darf den Tageswechsel kreuzen.
            default: // TIME_NIGHT
                p.primeHour         = RollBetween(22, 26) % 24;
                p.primeSpan         = RollBetween(3, 5);
                p.blockMinutes      = RollBetween(76, 210);
                p.commitReliability = RollBetween(50, 85);
                p.interruptibility  = RollBetween(10, 40);
                break;
        }

        // Daedalus 2005: Mittel 22,7 h/Woche, SD 14,1. Rechtsschief, also
        // unten kappen statt zu spiegeln.
        p.playMinutesWeek = RollNormal(int32(_cfg.playMinutesMean),
                                       int32(_cfg.playMinutesSpread),
                                       _cfg.playMinutesMin,
                                       _cfg.playMinutesMax);
    }

    // Schicht 3 wird NICHT gewuerfelt. Sie folgt aus Taetigkeit, Verlauf
    // und Gilde - sonst entstehen Kombinationen wie 'questor' + 'ticket',
    // die es nicht gibt. Wird bei jedem Stufenwechsel neu gesetzt.
    std::string DeriveGearMotive(Profile const& p, bool inGuild)
    {
        if (p.stage == "entry")
            return "means";
        if (p.stage == "burnout")
            return "means";   // das Motiv verliert seine Zugkraft

        if (inGuild && (p.archetype == "raider" ||
                        p.archetype == "dungeoneer"))
            return "duty";

        if (p.ambition >= 70 && (p.archetype == "collector" ||
                                 p.archetype == "pvper"))
            return "display";

        if (p.ambition >= 70)
            return "number";

        if (p.archetype == "raider" || p.archetype == "dungeoneer")
            return "ticket";

        return "means";
    }

    void RollLayers(Profile& p, ArchetypeDef const* def)
    {
        int32 mean   = _cfg.traitMean;
        int32 spread = _cfg.traitSpread;

        // Schicht 2 - vollstaendig unabhaengig vom Archetyp. Das ist die
        // eigentliche Reparatur; im Schema steht davon nichts.
        p.sociability       = RollNormal(mean, spread, 1, 99);
        p.ambition          = RollNormal(mean, spread, 1, 99);
        p.agreeableness     = RollNormal(mean, spread, 1, 99);
        p.honesty           = RollNormal(mean, spread, 1, 99);
        p.conscientiousness = RollNormal(mean, spread, 1, 99);
        p.sadism            = RollSadism();

        RollTimeProfile(p, def ? def->timeBias : TIME_NONE);

        p.stage      = "entry";
        p.stageSince = uint32(time(nullptr));
        p.anchorGuid = 0;   // wird verdient, nie gewuerfelt
        p.gearMotive = DeriveGearMotive(p, false);
    }

    Profile& EnsureProfile(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        auto it = _profiles.find(guid);
        if (it != _profiles.end())
        {
            Profile& old = it->second;

            // stage_since == 0 heisst: dieses Profil stammt aus der Zeit
            // vor den Schichten und hat nur die Spaltenvorgaben. Ohne das
            // hier saehen alle Altbots gleich aus - Vertraeglichkeit 50,
            // Fenster 18 Uhr, Block 60 - und der Roller wirkte kaputt.
            if (_cfg.layersEnable && old.stageSince == 0)
            {
                // Die Wanderung markiert die zwei entfallenen Archetypen,
                // weil ihre Taetigkeit nie erfasst wurde. Der Wesenszug
                // beziehungsweise das Zeitprofil dahinter soll aber
                // ueberleben - deshalb der Zusatz hinter dem Doppelpunkt.
                bool wasLoner  = old.archetype == "unset:loner";
                bool wasCasual = old.archetype == "unset:casual";

                if (old.archetype.rfind("unset", 0) == 0)
                    old.archetype = PickArchetype()->name;

                ArchetypeDef const* def = nullptr;
                for (ArchetypeDef const& a : kArchetypes)
                    if (old.archetype == a.name)
                        def = &a;

                RollLayers(old, def);

                // Was der alte Archetyp ueber den Bot wusste, nachtragen.
                //
                // Wichtig: BEREICHE, keine festen Zahlen. Feste Werte
                // machen aus jedem ehemaligen 'casual' denselben Menschen
                // - bei rund fuenfzehn Prozent der Bevoelkerung waere die
                // Zeitschicht dort wertlos.
                if (wasLoner && old.sociability > 25)
                    old.sociability = RollBetween(1, 25);

                if (wasCasual)
                {
                    if (old.blockMinutes > 60)
                        old.blockMinutes = RollBetween(25, 60);
                    if (old.playMinutesWeek > 800)
                        old.playMinutesWeek = RollBetween(240, 800);
                    if (old.commitReliability > 45)
                        old.commitReliability = RollBetween(15, 45);
                    if (old.interruptibility < 60)
                        old.interruptibility = RollBetween(60, 90);
                }

                old.dirty = true;
            }

            return old;
        }

        ArchetypeDef const* chosen = PickArchetype();

        Profile& p = _profiles[guid];
        p.archetype = chosen->name;

        // 1 = schwach, 5 = sehr gut. Die meisten sind Durchschnitt.
        uint32 tierRoll = RollBetween(1, 100);
        p.skillTier = tierRoll <= 10 ? 1
                    : tierRoll <= 30 ? 2
                    : tierRoll <= 70 ? 3
                    : tierRoll <= 92 ? 4 : 5;
        p.reputation = 0;

        if (_cfg.layersEnable)
            RollLayers(p, chosen);

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

    // Kein EnsureProfile: wer noch kein Profil hat (oder ein Mensch
    // ist), gilt als durchschnittlich gesellig statt eines zu bekommen.
    int32 SociabilityOf(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        auto it = _profiles.find(guid);
        return it == _profiles.end() ? 50 : int32(it->second.sociability);
    }

    // Wer kein Profil hat (oder ein Mensch ist), gilt als durchschnittlich
    // vertraeglich, statt eines zu bekommen.
    int32 AgreeablenessOf(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        auto it = _profiles.find(guid);
        return it == _profiles.end() ? 50 : int32(it->second.agreeableness);
    }

    // Wie viele Stunden zweier Zeitfenster sich ueberschneiden. Der Tag
    // ist ein Kreis: 22 Uhr plus vier Stunden reicht bis 2 Uhr frueh.
    uint32 WindowOverlap(uint32 h1, uint32 s1, uint32 h2, uint32 s2)
    {
        if (!s1 || !s2)
            return 0;

        bool slot[24] = { false };
        for (uint32 i = 0; i < s1 && i < 24; ++i)
            slot[(h1 + i) % 24] = true;

        uint32 n = 0;
        for (uint32 i = 0; i < s2 && i < 24; ++i)
            if (slot[(h2 + i) % 24])
                ++n;
        return n;
    }

    // Das Zeitfenster einer Gilde ist nicht gesetzt, sondern das, was ihre
    // Mitglieder daraus machen: die Stunde, zu der die meisten koennen,
    // ausgedehnt solange die Nachbarstunde noch halb so gut besetzt ist.
    void GuildWindow(std::vector<uint32> const& members,
                     uint32& hourOut, uint32& spanOut)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);

        uint32 hist[24] = { 0 };
        uint32 counted = 0;

        for (uint32 m : members)
        {
            auto it = _profiles.find(m);
            if (it == _profiles.end() || !it->second.primeSpan)
                continue;

            Profile const& p = it->second;
            for (uint32 i = 0; i < p.primeSpan && i < 24; ++i)
                ++hist[(p.primeHour + i) % 24];
            ++counted;
        }

        if (!counted)
        {
            hourOut = 18;
            spanOut = 3;
            return;
        }

        uint32 peak = 0;
        for (uint32 h = 1; h < 24; ++h)
            if (hist[h] > hist[peak])
                peak = h;

        uint32 const floorCount = hist[peak] / 2;
        uint32 start = peak;
        uint32 span  = 1;

        while (span < 24)
        {
            uint32 prev = (start + 23) % 24;
            uint32 next = (start + span) % 24;
            bool takePrev = hist[prev] > floorCount;
            bool takeNext = hist[next] > floorCount;

            if (!takePrev && !takeNext)
                break;

            if (takeNext && (!takePrev || hist[next] >= hist[prev]))
            {
                ++span;
            }
            else
            {
                start = prev;
                ++span;
            }
        }

        hourOut = start;
        spanOut = span;
    }

    uint32 InterruptibilityOf(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        auto it = _profiles.find(guid);
        return it == _profiles.end() ? 50u : it->second.interruptibility;
    }

    uint32 ConscientiousnessOf(uint32 guid)
    {
        std::lock_guard<std::recursive_mutex> guard(_graphMutex);
        auto it = _profiles.find(guid);
        return it == _profiles.end() ? 50u : it->second.conscientiousness;
    }

    // Wuerde dieser Bot ablehnen, weil ihm heute nicht nach Gesellschaft
    // ist? Das Ergebnis ist innerhalb eines Zeitfensters stabil - sonst
    // wuerde ein Einzelgaenger bei genug Anfragen doch in jeder Gruppe
    // landen, weil jeder neue Wurf eine neue Chance waere. So hat er
    // heute schlicht keine Lust, und morgen vielleicht wieder.
    bool SocialMoodRefuses(uint32 me, uint32 other, int32 companions,
                           bool inviting)
    {
        if (!_cfg.sociabilityEnable || !me || !other)
            return false;

        // Menschen entscheiden selbst, und dich lehnt niemand ab.
        if (IsHuman(me) || IsHuman(other))
            return false;

        int32 chance = SociabilityOf(me);

        // Wen man mag, mit dem geht man mit - auch ungern.
        if (_cfg.sociabilityAffinityDivisor > 0)
            chance += AffinityOf(me, other)
                    / _cfg.sociabilityAffinityDivisor;

        // Selbst zu fragen kostet mehr Ueberwindung als ja zu sagen.
        if (inviting)
            chance -= _cfg.sociabilityInviteMalus;

        // Je mehr Leute schon dabei sind, desto eher bleibt der Stille
        // lieber allein.
        if (companions > 0)
            chance -= companions * _cfg.sociabilityPerMemberMalus;

        if (chance < _cfg.sociabilityFloor)
            chance = _cfg.sociabilityFloor;
        if (chance >= 100)
            return false;

        // Stabiler Wurf: aus den beiden Kennungen und dem Zeitfenster.
        uint32 bucket = _cfg.sociabilityMoodHours
                      ? uint32(time(nullptr) / (_cfg.sociabilityMoodHours * 3600))
                      : 0;

        uint32 seed = me * 2654435761u;
        seed ^= other * 40503u;
        seed ^= bucket * 2246822519u;
        seed ^= seed >> 15;

        return int32(seed % 100u) >= chance;
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

    // ---- Gilden entstehen aus Cliquen -----------------------------------
    //
    // mod-playerbots kann Gilden beim Serverstart am Fliessband anlegen und
    // wahllos Bots hineinstecken. Fuer dieses Modul ist das der falsche
    // Weg herum: eine Gilde soll entstehen, WEIL sich Leute kennen, nicht
    // damit es Gilden gibt. AiPlayerbot.RandomBotGuildCount gehoert auf 0.
    //
    // Der Zeitgeber ist hier nur das Nachsehen. Ob gegruendet wird,
    // entscheidet die Lage: ein Bot mit Ehrgeiz, der genug Leute kennt,
    // die IHN auch moegen und einander vertragen - und die gerade
    // zusammen online sind.

    void RegisterBotGuilds();   // definiert unter FoundTick

    // ---- Gildennamen ----------------------------------------------------
    //
    // Recherchiert statt erfunden: auf deutschen Realms sind englische
    // Gildennamen voellig ueblich und waren es immer ("Touch of Destiny"),
    // neben deutschen Lore-Namen ("Sternenwind", "Arme der Finsternis")
    // und alltaeglichem Humor ("AFK Bier holen"). Ein Server, auf dem
    // alles gleich klingt, wirkt gebaut.
    //
    // Harte Grenze des Spiels: 2 bis 24 Zeichen, keine Ziffern, keine
    // Sonderzeichen. Leerzeichen und Umlaute sind erlaubt.

    // Adjektivstamm ohne Endung - die haengt an der Zahl des Substantivs:
    // "Die Eiserne Front", aber "Die Eisernen Klingen".
    char const* kDeAdj[] = {
        "Eisern", "Still", "Letzt", "Frei", "Alt", "Rot", "Schwarz",
        "Golden", "Kalt", "Wild", "Fern", "Treu", "Grau", "Zweit",
        "Stolz", "Blass", "Ewig", "Dunkl", "Weiß", "Jung",
    };

    struct DeNoun { char const* word; bool plural; };

    DeNoun const kDeRaid[] = {
        { "Front", false }, { "Faust", false }, { "Wache", false },
        { "Klingen", true }, { "Legion", false }, { "Bastion", false },
    };
    DeNoun const kDePvp[] = {
        { "Meute", false }, { "Jäger", true }, { "Krallen", true },
        { "Wölfe", true }, { "Fehde", false }, { "Klingen", true },
    };
    DeNoun const kDeGath[] = {
        { "Zunft", false }, { "Kammer", false }, { "Waage", false },
        { "Kompanie", false }, { "Händler", true }, { "Werkstatt", false },
    };
    DeNoun const kDeExpl[] = {
        { "Pfade", true }, { "Wanderer", true }, { "Weite", false },
        { "Fernsicht", false }, { "Sucher", true }, { "Karte", false },
    };
    DeNoun const kDeAny[] = {
        { "Runde", false }, { "Schar", false }, { "Gilde", false },
        { "Bande", false }, { "Gefährten", true }, { "Sippe", false },
    };

    char const* kDeHead[] = {
        "Orden", "Ritter", "Wächter", "Arme", "Kinder", "Erben", "Hüter",
        "Schatten", "Klingen", "Sucher", "Zeichen", "Stimmen",
    };
    char const* kDeTail[] = {
        "des Phönix", "des Lichts", "der Nacht", "der Finsternis",
        "des Sturms", "der Asche", "der Flamme", "der Krone",
        "des Nordens", "der Ewigkeit", "des Zwielichts", "der Stille",
    };

    char const* kEnAdj[] = {
        "Eternal", "Silent", "Last", "Iron", "Crimson", "Golden", "Frozen",
        "Wild", "Distant", "Grey", "Second", "Fallen", "Ashen", "Hollow",
        "Pale", "Sworn",
    };
    char const* kEnNoun[] = {
        "Legion", "Blades", "Watch", "Company", "Order", "Vanguard",
        "Wolves", "Crown", "Ashes", "Path", "Circle", "Banner",
    };
    char const* kEnHead[] = {
        "Touch", "Sons", "Heirs", "Children", "Keepers", "Shadows",
        "Blades", "Wings", "Echoes", "Signs",
    };
    char const* kEnTail[] = {
        "of Destiny", "of Ash", "of the North", "of Dawn", "of Silence",
        "of the Storm", "of Winter", "of Embers", "of the Vale",
        "of Sorrow",
    };

    char const* kSolo[] = {
        "Zeitlos", "Sternenwind", "Nemesis", "Aurora", "Vigil", "Requiem",
        "Zenit", "Eclipse", "Nachtwind", "Morgenrot", "Ascension",
        "Solstice", "Dämmerwache", "Abendrot", "Vermächtnis", "Sanctum",
    };

    char const* kFun[] = {
        "AFK Bier holen", "Nur kurz Pause", "Rentnergang", "Pyjamahelden",
        "Kaffee zuerst", "Schnitzelkrieger", "Sofasoldaten",
        "Wipe Akademie", "Heiler vergessen", "Zu zweit allein",
        "Kekse für alle", "Randgruppe", "Gleich Feierabend", "Ohne Plan",
        "Wir üben noch", "Montags nie",
    };

    template<size_t N>
    char const* PickOne(char const* const (&a)[N])
    {
        return a[RollBetween(0, uint32(N) - 1)];
    }

    template<typename T, size_t N>
    T const& PickRow(T const (&a)[N])
    {
        return a[RollBetween(0, uint32(N) - 1)];
    }

    // Das Spiel zaehlt Zeichen, nicht Bytes - ein Umlaut ist in UTF-8
    // zwei Bytes lang. Ohne das hier waeren Namen mit Umlauten
    // faelschlich zu lang.
    size_t NameLength(std::string const& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80)
                ++n;
        return n;
    }

    std::string ComposeGuildName(std::string const& archetype)
    {
        uint32 roll = RollBetween(1, 100);

        if (roll <= _cfg.nameHumorPercent)
            return PickOne(kFun);

        if (roll <= _cfg.nameHumorPercent + _cfg.nameEnglishPercent)
            return RollBetween(0, 1)
                 ? std::string(PickOne(kEnAdj)) + " " + PickOne(kEnNoun)
                 : std::string(PickOne(kEnHead)) + " " + PickOne(kEnTail);

        switch (RollBetween(0, 2))
        {
            case 0:
            {
                DeNoun n = (archetype == "raider"
                            || archetype == "dungeoneer") ? PickRow(kDeRaid)
                         : archetype == "pvper"           ? PickRow(kDePvp)
                         : (archetype == "gatherer"
                            || archetype == "collector")  ? PickRow(kDeGath)
                         : (archetype == "explorer"
                            || archetype == "questor")    ? PickRow(kDeExpl)
                                                          : PickRow(kDeAny);

                return std::string("Die ") + PickOne(kDeAdj)
                     + (n.plural ? "en " : "e ") + n.word;
            }
            case 1:
                return std::string(PickOne(kDeHead)) + " "
                     + PickOne(kDeTail);
            default:
                return PickOne(kSolo);
        }
    }

    std::string MakeGuildName(std::string const& archetype)
    {
        // Zwanzig Versuche. Ueber siebenhundert verschiedene Namen sind
        // moeglich; wer keinen freien findet, gruendet in der naechsten
        // Runde.
        for (uint32 tries = 0; tries < 20; ++tries)
        {
            std::string name = ComposeGuildName(archetype);

            size_t len = NameLength(name);
            if (len < 2 || len > 24)
                continue;

            if (!sGuildMgr->GetGuildByName(name))
                return name;
        }
        return std::string();
    }

    uint32 CountBotGuilds()
    {
        QueryResult res = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM bot_social_guild");
        return res ? uint32(res->Fetch()[0].Get<uint64>()) : 0u;
    }

    void FoundTick()
    {
        if (!_cfg.foundEnable || !_cfg.enable)
            return;

        time_t now = time(nullptr);
        if (_lastFound
            && uint32(now - _lastFound) < _cfg.foundServerCooldown)
            return;

        if (_cfg.foundMaxGuilds && CountBotGuilds() >= _cfg.foundMaxGuilds)
            return;

        int32 const minBond = _cfg.foundMinBond ? _cfg.foundMinBond
                                                : _cfg.friendsThreshold;

        // Wer koennte gruenden, und wer waere gerade greifbar?
        std::vector<uint32> free[2];   // je Fraktion
        std::vector<uint32> founders[2];

        {
            std::shared_lock<std::shared_mutex> lock(
                *HashMapHolder<Player>::GetLock());

            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* p = pair.second;
                if (!p || !p->IsInWorld() || !IsBot(p) || p->GetGuildId())
                    continue;
                if (p->GetLevel() < _cfg.foundMinLevel)
                    continue;

                uint32 low  = p->GetGUID().GetCounter();
                uint32 side = p->GetTeamId() == TEAM_ALLIANCE ? 0u : 1u;

                free[side].push_back(low);

                if (SociabilityOf(low) < int32(_cfg.foundMinSociability))
                    continue;
                if (ReputationOf(low) < _cfg.recruitMinReputation)
                    continue;

                founders[side].push_back(low);
            }
        }

        uint32 bestFounder = 0;
        std::vector<uint32> bestCircle;

        for (uint32 side = 0; side < 2; ++side)
        {
            for (uint32 f : founders[side])
            {
                if (EnsureProfile(f).ambition < _cfg.foundMinAmbition)
                    continue;

                // Nur wer IHN auch mag. Einseitige Zuneigung gruendet
                // keine Gilde - das waere ein Fanclub, kein Verein.
                std::vector<uint32> circle;
                for (uint32 o : free[side])
                {
                    if (o == f)
                        continue;
                    if (AffinityOf(f, o) < minBond)
                        continue;
                    if (AffinityOf(o, f) < minBond)
                        continue;
                    circle.push_back(o);
                }

                if (circle.size() < _cfg.foundMinFriends)
                    continue;

                // Und die Runde muss sich untereinander vertragen. Zwei
                // Leute, die einander meiden, gruenden nichts zusammen.
                bool feud = false;
                for (size_t i = 0; i < circle.size() && !feud; ++i)
                    for (size_t j = i + 1; j < circle.size(); ++j)
                        if (AffinityOf(circle[i], circle[j])
                                <= _cfg.avoidThreshold
                            || AffinityOf(circle[j], circle[i])
                                <= _cfg.avoidThreshold)
                        {
                            feud = true;
                            break;
                        }

                if (feud)
                    continue;

                if (circle.size() > bestCircle.size())
                {
                    bestCircle = circle;
                    bestFounder = f;
                }
            }
        }

        // Ohne das hier ist "es passiert nichts" nicht von "es geht nicht"
        // zu unterscheiden - und genau daran haben wir diese Woche schon
        // zweimal Zeit verloren.
        if (_cfg.debug)
            LOG_INFO("module",
                     "BotSocial/Gruendung: {} gildenlose Bots online, "
                     "{} davon kaemen als Gruender in Frage, "
                     "beste Runde {} Bekannte (noetig {}), Schwelle {}.",
                     free[0].size() + free[1].size(),
                     founders[0].size() + founders[1].size(),
                     bestCircle.size(), _cfg.foundMinFriends, minBond);

        if (!bestFounder)
            return;

        Player* leader = ObjectAccessor::FindPlayer(MakeGuid(bestFounder));
        if (!leader || !leader->IsInWorld() || leader->GetGuildId())
            return;

        std::string name = MakeGuildName(ArchetypeOf(bestFounder));
        if (name.empty())
            return;

        Guild* guild = new Guild();
        if (!guild->Create(leader, name))
        {
            delete guild;
            return;
        }
        sGuildMgr->AddGuild(guild);

        uint32 joined = 0;
        for (uint32 m : bestCircle)
        {
            Player* mp = ObjectAccessor::FindPlayer(MakeGuid(m));
            if (!mp || !mp->IsInWorld() || mp->GetGuildId())
                continue;
            if (guild->AddMember(mp->GetGUID()))
                ++joined;
        }

        _lastFound = now;

        // Sofort aufnehmen, sonst wirbt bis zur naechsten Werberunde
        // niemand fuer die frische Gilde.
        RegisterBotGuilds();

        LogEvent(bestFounder, 0, "guild_founded", name, int32(joined));

        LOG_INFO("module",
                 "BotSocial: {} gruendete '{}' mit {} Bekannten",
                 leader->GetName(), name, joined);
    }

    // Bot-gefuehrte Gilden in bot_social_guild aufnehmen und ihnen eine
    // Zielgroesse geben.
    //
    // Lief bisher nur beim Serverstart. Gilden, die mod-playerbots im
    // laufenden Betrieb gruendet, blieben damit unbekannt und wurden nie
    // beworben - bis zum naechsten Neustart. Deshalb jetzt auch in jeder
    // Werberunde; die Tabelle ist klein und INSERT IGNORE billig.
    void RegisterBotGuilds()
    {
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO bot_social_guild (guild_id, target_size) "
            "SELECT g.guildid, {} FROM guild g "
            "JOIN characters c ON c.guid = g.leaderguid "
            "JOIN {}.account a ON a.id = c.account "
            "WHERE a.username LIKE '{}%'",
            _cfg.guildTargetMin, _cfg.authDatabase, _cfg.botAccountPrefix);

        // Frisch aufgenommene Zeilen tragen die Mindestgroesse als Marke
        // und bekommen hier ihre eigene Zielgroesse.
        QueryResult res = CharacterDatabase.Query(
            "SELECT guild_id FROM bot_social_guild WHERE target_size = {}",
            _cfg.guildTargetMin);

        if (!res)
            return;

        do
        {
            CharacterDatabase.Execute(
                "UPDATE bot_social_guild SET target_size = {} "
                "WHERE guild_id = {}",
                RollBetween(_cfg.guildTargetMin, _cfg.guildTargetMax),
                res->Fetch()[0].Get<uint32>());
        } while (res->NextRow());
    }

    uint32 PickRecruit(Guild* guild, TeamId team,
                       uint32 gHour = 0, uint32 gSpan = 0)
    {
        std::vector<uint32> members = GuildMemberGuids(guild->GetId());
        if (members.empty())
            return 0;

        std::vector<uint32> candidates;

        // WICHTIG: nicht ueber die Sitzungsliste laufen. mod-playerbots
        // meldet seine Bot-Sitzungen nie beim WorldSessionMgr an - dort
        // stehen nur echte Spieler. Die Spielerliste des Kerns enthaelt
        // dagegen jeden Player in der Welt, Bots eingeschlossen.
        {
        std::shared_lock<std::shared_mutex> lock(
            *HashMapHolder<Player>::GetLock());

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || !IsBot(p))
                continue;
            if (p->GetGuildId() || p->GetTeamId() != team)
                continue;

            uint32 low = p->GetGUID().GetCounter();

            // Manche treten schlicht nichts bei. Frueher stand hier der
            // Archetyp 'loner' - den gibt es seit dem Schichten-Umbau
            // nicht mehr, der Filter lief seither ins Leere. Es war nie
            // eine Taetigkeit, sondern ein Wesenszug.
            if (SociabilityOf(low) <= int32(_cfg.recruitMinSociability))
                continue;

            // A bad name travels ahead of you.
            if (ReputationOf(low) < _cfg.recruitMinReputation)
                continue;

            candidates.push_back(low);
        }
        }   // Sperre wieder frei - ab hier wird gerechnet und gehandelt

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

            // Wer zur selben Zeit spielt, wird eher genommen. Nicht weil
            // er beliebter waere, sondern weil man ihn ueberhaupt trifft.
            if (_cfg.guildWindowEnable && gSpan)
            {
                std::lock_guard<std::recursive_mutex> guard(_graphMutex);
                auto it = _profiles.find(c);
                if (it != _profiles.end())
                    score += int32(WindowOverlap(gHour, gSpan,
                                                 it->second.primeHour,
                                                 it->second.primeSpan))
                           * _cfg.recruitWindowBonus;
            }

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

        // Neu gegruendete Gilden zuerst aufnehmen - sonst wirbt niemand
        // fuer sie, bis der Server das naechste Mal neu startet.
        RegisterBotGuilds();

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

            // Das Zeitfenster der Gilde aus ihren Mitgliedern ableiten
            // und mitschreiben - das Dashboard und die Spaltung lesen es.
            uint32 gHour = 0, gSpan = 0;
            if (_cfg.guildWindowEnable)
            {
                GuildWindow(GuildMemberGuids(guildId), gHour, gSpan);
                CharacterDatabase.Execute(
                    "UPDATE bot_social_guild SET prime_hour = {}, "
                    "prime_span = {} WHERE guild_id = {}",
                    gHour, gSpan, guildId);
            }

            uint32 recruit = PickRecruit(guild, leader->GetTeamId(),
                                         gHour, gSpan);
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

        // Erst einsammeln, wer ueberhaupt in Frage kommt - die
        // Rangaenderung selbst passiert danach ohne gehaltene Sperre.
        // (Bots stehen nicht im WorldSessionMgr, siehe Anwerbung.)
        std::vector<ObjectGuid> promotable;
        {
            std::shared_lock<std::shared_mutex> lock(
                *HashMapHolder<Player>::GetLock());

            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* p = pair.second;
                if (!p || !p->IsInWorld() || !IsBot(p))
                    continue;
                if (!p->GetGuildId())
                    continue;

                promotable.push_back(p->GetGUID());
            }
        }

        for (ObjectGuid const& guid : promotable)
        {
            Player* p = ObjectAccessor::FindPlayer(guid);
            if (!p || !p->IsInWorld())
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

            // Williams 2006: der haeufigste Austrittsgrund ist nicht
            // Streit, sondern dass die eigenen Ziele nicht mehr zu denen
            // der Gilde passen. Das Zeitfenster ist der Teil davon, den
            // wir messen koennen.
            uint32 gHour = 0, gSpan = 0;
            if (_cfg.guildWindowEnable)
                GuildWindow(members, gHour, gSpan);

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

                // Wer zu ganz anderen Zeiten spielt als seine Gilde,
                // braucht weniger Rueckhalt, um zu gehen - er hat ohnehin
                // wenig von ihr.
                if (_cfg.guildWindowEnable && gSpan
                    && WindowOverlap(gHour, gSpan, prof.primeHour,
                                     prof.primeSpan)
                       <= _cfg.schismWindowGapMax)
                    clique += _cfg.schismTimeBonus;

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

    _cfg.sociabilityEnable            = sConfigMgr->GetOption<bool>("BotSocial.Sociability.Enable", false);
    _cfg.sociabilityInviteMalus       = sConfigMgr->GetOption<int32>("BotSocial.Sociability.InviteMalus", 10);
    _cfg.sociabilityPerMemberMalus    = sConfigMgr->GetOption<int32>("BotSocial.Sociability.PerMemberMalus", 3);
    _cfg.sociabilityAffinityDivisor   = sConfigMgr->GetOption<int32>("BotSocial.Sociability.AffinityDivisor", 4);
    _cfg.sociabilityMoodHours         = sConfigMgr->GetOption<uint32>("BotSocial.Sociability.MoodHours", 6);
    _cfg.sociabilityFloor             = sConfigMgr->GetOption<int32>("BotSocial.Sociability.Floor", 5);

    // ---- Schichten (Fassung 2) ----
    _cfg.layersEnable        = sConfigMgr->GetOption<bool>("BotSocial.Layers.Enable", true);
    _cfg.traitMean           = sConfigMgr->GetOption<int32>("BotSocial.Traits.Mean", 50);
    _cfg.traitSpread         = sConfigMgr->GetOption<int32>("BotSocial.Traits.Spread", 18);
    _cfg.sadismTailPercent   = sConfigMgr->GetOption<uint32>("BotSocial.Sadism.TailPercent", 5);
    _cfg.sadismQuietMax      = sConfigMgr->GetOption<uint32>("BotSocial.Sadism.QuietMax", 15);
    _cfg.sadismTailLo        = sConfigMgr->GetOption<uint32>("BotSocial.Sadism.TailFloor", 40);
    _cfg.timeWeightEvening   = sConfigMgr->GetOption<uint32>("BotSocial.Time.WeightEvening", 40);
    _cfg.timeWeightAlongside = sConfigMgr->GetOption<uint32>("BotSocial.Time.WeightAlongside", 35);
    _cfg.timeWeightHeavy     = sConfigMgr->GetOption<uint32>("BotSocial.Time.WeightHeavy", 13);
    _cfg.timeWeightNight     = sConfigMgr->GetOption<uint32>("BotSocial.Time.WeightNight", 12);
    _cfg.playMinutesMean     = sConfigMgr->GetOption<uint32>("BotSocial.Time.MinutesMean", 1360);
    _cfg.playMinutesSpread   = sConfigMgr->GetOption<uint32>("BotSocial.Time.MinutesSpread", 846);
    _cfg.playMinutesMin      = sConfigMgr->GetOption<uint32>("BotSocial.Time.MinutesMin", 180);
    _cfg.playMinutesMax      = sConfigMgr->GetOption<uint32>("BotSocial.Time.MinutesMax", 4200);
    _cfg.instanceMinBlock     = sConfigMgr->GetOption<uint32>("BotSocial.Time.InstanceMinBlock", 40);
    _cfg.instanceMaxInterrupt = sConfigMgr->GetOption<uint32>("BotSocial.Time.InstanceMaxInterrupt", 80);
    _cfg.anchorMinDungeons   = sConfigMgr->GetOption<uint32>("BotSocial.Anchor.MinDungeons", 4);
    _cfg.anchorMinMinutes    = sConfigMgr->GetOption<uint32>("BotSocial.Anchor.MinMinutes", 240);
    _cfg.flushMaxStatements  = sConfigMgr->GetOption<uint32>("BotSocial.FlushMaxStatements", 2000);
    _cfg.traitsAffect        = sConfigMgr->GetOption<bool>("BotSocial.Traits.Affect", true);
    _cfg.traitGrudgeSwing    = sConfigMgr->GetOption<int32>("BotSocial.Traits.GrudgeSwing", 80);
    _cfg.traitAvoidSwing     = sConfigMgr->GetOption<int32>("BotSocial.Traits.AvoidSwing", 40);
    _cfg.traitForgiveFrom    = sConfigMgr->GetOption<uint32>("BotSocial.Traits.ForgiveFrom", 70);
    _cfg.traitGrudgeHoldBelow = sConfigMgr->GetOption<uint32>("BotSocial.Traits.GrudgeHoldBelow", 30);
    _cfg.anchorEnable        = sConfigMgr->GetOption<bool>("BotSocial.Anchor.Enable", true);
    _cfg.anchorBreakAt       = sConfigMgr->GetOption<int32>("BotSocial.Anchor.BreakAt", 40);
    _cfg.bailReasonEnable    = sConfigMgr->GetOption<bool>("BotSocial.Bail.ReasonEnable", true);
    _cfg.bailExcuseFrom      = sConfigMgr->GetOption<uint32>("BotSocial.Bail.ExcuseFrom", 70);
    _cfg.bailExcusePercent   = sConfigMgr->GetOption<int32>("BotSocial.Bail.ExcusePercent", 40);
    _cfg.bailAggravateBelow  = sConfigMgr->GetOption<uint32>("BotSocial.Bail.AggravateBelow", 30);
    _cfg.bailAggravatePercent = sConfigMgr->GetOption<int32>("BotSocial.Bail.AggravatePercent", 150);
    _cfg.foundEnable         = sConfigMgr->GetOption<bool>("BotSocial.Found.Enable", true);
    _cfg.foundInterval       = sConfigMgr->GetOption<uint32>("BotSocial.Found.Interval", 900);
    _cfg.foundServerCooldown = sConfigMgr->GetOption<uint32>("BotSocial.Found.ServerCooldown", 3600);
    _cfg.foundMaxGuilds      = sConfigMgr->GetOption<uint32>("BotSocial.Found.MaxGuilds", 60);
    _cfg.foundMinFriends     = sConfigMgr->GetOption<uint32>("BotSocial.Found.MinFriends", 4);
    _cfg.foundMinBond        = sConfigMgr->GetOption<int32>("BotSocial.Found.MinBond", 80);
    _cfg.foundMinAmbition    = sConfigMgr->GetOption<uint32>("BotSocial.Found.MinAmbition", 60);
    _cfg.foundMinSociability = sConfigMgr->GetOption<uint32>("BotSocial.Found.MinSociability", 45);
    _cfg.foundMinLevel       = sConfigMgr->GetOption<uint32>("BotSocial.Found.MinLevel", 10);
    _cfg.nameEnglishPercent  = sConfigMgr->GetOption<uint32>("BotSocial.Found.NameEnglishPercent", 40);
    _cfg.nameHumorPercent    = sConfigMgr->GetOption<uint32>("BotSocial.Found.NameHumorPercent", 15);
    if (_cfg.nameEnglishPercent + _cfg.nameHumorPercent > 100)
    {
        LOG_WARN("module", "BotSocial: NameEnglishPercent + NameHumorPercent "
                 "ueber 100 - deutsche Namen kaemen nie vor. Auf 40/15 gesetzt.");
        _cfg.nameEnglishPercent = 40;
        _cfg.nameHumorPercent   = 15;
    }
    _cfg.guildWindowEnable   = sConfigMgr->GetOption<bool>("BotSocial.GuildWindow.Enable", true);
    _cfg.recruitWindowBonus  = sConfigMgr->GetOption<int32>("BotSocial.Recruit.WindowBonus", 8);
    _cfg.recruitMinSociability = sConfigMgr->GetOption<uint32>("BotSocial.Recruit.MinSociability", 20);
    _cfg.schismWindowGapMax  = sConfigMgr->GetOption<uint32>("BotSocial.Schism.WindowGapMax", 1);
    _cfg.schismTimeBonus     = sConfigMgr->GetOption<int32>("BotSocial.Schism.TimeBonus", 40);

    // Ein Wert von 0 bei der Streuung macht aus jedem Bot denselben
    // Menschen. Das ist fast nie gewollt, also unten kappen.
    if (_cfg.traitSpread < 1)
        _cfg.traitSpread = 1;
    if (_cfg.sadismQuietMax > 100)
        _cfg.sadismQuietMax = 100;
    if (_cfg.playMinutesMin > _cfg.playMinutesMax)
        _cfg.playMinutesMin = _cfg.playMinutesMax;

    _cfg.gossipEnable       = sConfigMgr->GetOption<bool>("BotSocial.Gossip.Enable", false);
    _cfg.gossipMinSeverity  = sConfigMgr->GetOption<int32>("BotSocial.Gossip.MinSeverity", 25);
    _cfg.gossipMinTrust     = sConfigMgr->GetOption<int32>("BotSocial.Gossip.MinTrust", 40);
    _cfg.gossipFullTrust    = sConfigMgr->GetOption<int32>("BotSocial.Gossip.FullTrust", 120);
    _cfg.gossipPercent      = sConfigMgr->GetOption<int32>("BotSocial.Gossip.Percent", 25);
    _cfg.gossipMaxListeners = sConfigMgr->GetOption<uint32>("BotSocial.Gossip.MaxListeners", 5);
    _cfg.gossipMaxStrangers = sConfigMgr->GetOption<uint32>("BotSocial.Gossip.MaxStrangers", 2);
    _cfg.gossipDecayFactor  = sConfigMgr->GetOption<uint32>("BotSocial.Gossip.DecayFactor", 3);
    _cfg.gossipAboutPlayers = sConfigMgr->GetOption<bool>("BotSocial.Gossip.AboutPlayers", false);

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
        int32       amount = _cfg.lossBailed;
        std::string kind   = "bailed";
        std::string detail = "Instanz verlassen";

        // Der Unterschied, den das Modell bisher nicht treffen konnte:
        // der nette Unzuverlaessige gegen das zuverlaessige Ekel. Beide
        // gehen mitten im Dungeon, beide sammeln Groll - nur der Grund
        // unterscheidet sie, und Gruppen bewerten ihn verschieden.
        if (_cfg.bailReasonEnable)
        {
            uint32 intr = InterruptibilityOf(leaver);
            uint32 con  = ConscientiousnessOf(leaver);

            if (intr >= _cfg.bailExcuseFrom)
            {
                amount = amount * _cfg.bailExcusePercent / 100;
                kind   = "bailed_life";
                detail = "Instanz verlassen (musste weg)";
            }
            else if (con <= _cfg.bailAggravateBelow)
            {
                amount = amount * _cfg.bailAggravatePercent / 100;
                detail = "Instanz verlassen (einfach gegangen)";
            }

            if (amount < 1)
                amount = 1;   // ganz folgenlos bleibt es nie
        }

        GroupGrudge(group, leaver, amount, kind, detail);
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
    SpreadRumour(low, master, _cfg.lossGuildKick, "guild_kick",
                 player->GetMapId(), player->GetZoneId());
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

    SpreadRumour(killed->GetGUID().GetCounter(),
                 killer->GetGUID().GetCounter(),
                 _cfg.lossGank, "gank",
                 killed->GetMapId(), killed->GetZoneId());

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
    if (!_cfg.enable || !player || !group)
        return false;

    uint32 me = player->GetGUID().GetCounter();

    std::vector<uint32> members = GroupMemberLowGuids(group);

    if (_cfg.avoidEnable)
    {
        // Ein unvertraeglicher Bot lehnt frueher ab, ein sanfter haelt
        // laenger aus. Die Schwelle ist negativ; ein Aufschlag macht sie
        // strenger. Haengt nur am Bot selbst, also einmal vor der Schleife.
        int32 threshold = _cfg.avoidThreshold;
        if (_cfg.traitsAffect)
        {
            threshold += (50 - AgreeablenessOf(me))
                       * _cfg.traitAvoidSwing / 100;
            if (threshold > -1)
                threshold = -1;
        }

        for (uint32 other : members)
        {
            if (other == me)
                continue;

            // Never refuse because of the human. You are always welcome.
            if (IsHuman(other) && !_cfg.playerCanBeIgnored)
                continue;

            if (AffinityOf(me, other) <= threshold)
                return true;
        }
    }

    // Niemanden zu hassen heisst nicht, mitgehen zu wollen. Wie voll die
    // Gruppe schon ist, zaehlt mit - und wenn ein Mensch dabei ist, wird
    // grundsaetzlich nicht abgelehnt.
    if (_cfg.sociabilityEnable)
    {
        int32  companions = 0;
        uint32 anchorMate = 0;

        for (uint32 other : members)
        {
            if (other == me)
                continue;
            if (IsHuman(other))
                return false;

            ++companions;

            // Bezugspunkt fuer den Wurf ist der Bot, den dieser hier am
            // liebsten mag - mit einem Freund in der Gruppe faellt das
            // Ja leichter.
            if (!anchorMate
                || AffinityOf(me, other) > AffinityOf(me, anchorMate))
                anchorMate = other;
        }

        if (anchorMate
            && SocialMoodRefuses(me, anchorMate, companions - 1, false))
            return true;
    }

    return false;
}

bool WouldRefuseInvite(Player* player, std::string const& targetName)
{
    if (!_cfg.enable || !player || targetName.empty())
        return false;
    if (!_cfg.avoidEnable && !_cfg.sociabilityEnable)
        return false;

    uint32 target = 0;
    if (!ResolveCharacter(targetName, target))
        return false;

    if (IsHuman(target) && !_cfg.playerCanBeIgnored)
        return false;

    uint32 me = player->GetGUID().GetCounter();

    if (_cfg.avoidEnable
        && AffinityOf(me, target) <= _cfg.avoidThreshold)
        return true;

    // Selbst jemanden anzusprechen kostet mehr Ueberwindung, als auf
    // eine Einladung ja zu sagen - deshalb inviting = true.
    return SocialMoodRefuses(me, target, 0, true);
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

    RegisterBotGuilds();

    // Rows seeded before the cap existed (or before it was lowered) still carry oversized targets.
    // RecruitTick reads target_size per row, not the config, so those guilds would keep recruiting
    // past the ceiling until re-rolled. Bring them down in place.
    if (_cfg.guildCapacityCap)
        CharacterDatabase.Execute(
            "UPDATE bot_social_guild SET target_size = {} WHERE target_size > {}",
            _cfg.guildTargetMax, _cfg.guildTargetMax);

    // Wir schreiben NICHT in playerbots.conf - das ist die Konfiguration
    // eines fremden Moduls, und etwas, das ungefragt fremde Einstellungen
    // aendert, ist die Sorte Ueberraschung, die man erst Wochen spaeter
    // findet. Stattdessen sagen wir laut Bescheid, wenn sie dem hier
    // widerspricht.
    if (_cfg.foundEnable)
    {
        uint32 pbGuilds =
            sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotGuildCount", 0);

        if (pbGuilds)
            LOG_ERROR("module",
                      "mod-bot-social: AiPlayerbot.RandomBotGuildCount = {}. "
                      "mod-playerbots legt damit beim Serverstart {} Gilden "
                      "an und steckt beliebige Bots hinein - auch auf Stufe 1, "
                      "ohne dass die sich je begegnet waeren. Genau das soll "
                      "BotSocial.Found ersetzen. Setze den Wert in "
                      "playerbots.conf auf 0, oder schalte BotSocial.Found."
                      "Enable aus - beides zusammen ergibt keinen Sinn.",
                      pbGuilds, pbGuilds);
        else
            LOG_INFO("module",
                     "mod-bot-social: Gilden entstehen aus Cliquen - "
                     "hoechstens {} GILDEN auf dem Server, je hoechstens {} "
                     "Mitglieder, fruehestens alle {} Minuten eine neue.",
                     _cfg.foundMaxGuilds, _cfg.guildTargetMax,
                     _cfg.foundServerCooldown / 60);
    }

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

    _foundTimer += diff;
    if (_foundTimer >= _cfg.foundInterval * IN_MILLISECONDS)
    {
        _foundTimer = 0;
        FoundTick();
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
    row.agreeableness     = it->second.agreeableness;
    row.honesty           = it->second.honesty;
    row.conscientiousness = it->second.conscientiousness;
    row.sadism            = it->second.sadism;
    row.gearMotive        = it->second.gearMotive;
    row.playMinutesWeek   = it->second.playMinutesWeek;
    row.primeHour         = it->second.primeHour;
    row.primeSpan         = it->second.primeSpan;
    row.blockMinutes      = it->second.blockMinutes;
    row.commitReliability = it->second.commitReliability;
    row.interruptibility  = it->second.interruptibility;
    row.stage             = it->second.stage;
    row.anchorGuid        = it->second.anchorGuid;

    // Eine einzelne Abfrage nur, wenn es einen Anker gibt. Ohne Anker
    // (der Normalfall) kostet .social who keine zusaetzliche Runde.
    if (row.anchorGuid)
    {
        QueryResult nres = CharacterDatabase.Query(
            "SELECT name FROM characters WHERE guid = {}", row.anchorGuid);
        row.anchorName = nres ? (*nres)[0].Get<std::string>() : std::string("?");
    }
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
