# mod-bot-social

A persistent social graph for the bots from
[mod-playerbots](https://github.com/liyunfan1223/mod-playerbots). Bots
remember who they played with, and act on it: they make friends, hold
grudges, recruit for their guild, promote each other, and split away.

For AzerothCore, WotLK (3.3.5a).

*Deutsche Fassung weiter unten.*

## The idea behind it

The model deliberately mirrors a few things people actually do:

- **Affection is one-sided.** Two rows per pair, with different numbers. A
  can like B while B thinks nothing of A.
- **Grudges fade slower than friendships.** Negative values decay only a
  third as often as positive ones.
- **The first few encounters count double.** A first impression weighs more
  than the twentieth.
- **Nobody keeps track of eight hundred people.** A Dunbar cap trims the
  weakest bonds — and keeps the table from growing quadratically.
- **People who want to play differently rub each other the wrong way.**
  Every bot gets an activity (questor, dungeoneer, raider, pvper,
  gatherer, explorer, collector); clashing pairs take a small penalty per
  shared group.

The graph lives in memory; the database is the backup.

**The module does not link against mod-playerbots.** It only observes core
script hooks and recognises bots through `WorldSession::IsBot()`. An update
to mod-playerbots therefore cannot break it.

## The six layers (2.0)

Up to 1.x a bot was one number: an archetype, and everything else followed
from it. A `casual` was automatically less sociable, a `raider` more
ambitious. That is the one thing the literature does not support — what
somebody plays says very little about what they are like. Two of the seven
archetypes, `casual` and `loner`, were not activities at all; they were a
schedule and a temperament wearing an activity's clothes.

2.0 pulls them apart. A bot is now six independent layers:

| Layer | What it is | Where it comes from |
|---|---|---|
| 1 — Activity | questor, dungeoneer, raider, pvper, gatherer, explorer, collector | weighted roll |
| 2 — Character | agreeableness, honesty, conscientiousness, sociability, ambition, sadism | rolled, independent of layer 1 |
| 3 — Gear motive | means, number, display, ticket, duty | **derived**, never rolled |
| 4 — Availability | prime hour, span, block length, reliability, interruptibility | rolled **jointly** |
| 5 — Career stage | entry, practice, mastery, burnout, recovery | event-driven |
| 6 — Anchor | the one person someone keeps logging in for | earned, never rolled |

Three of these deserve a note, because each hides a trap.

**Layer 2 is not one bell curve.** Four traits are normal around 50.
Sadism is not: Volkmer et al. (2023, preregistered, N = 1026) show by
quantile regression that sadism explains only the *upper* quantiles of
trolling — for low values it says nothing. So the mass sits near zero and a
thin tail runs high. The first implementation drew from two separate pots
and left nothing at all between 16 and 39; in a live population of 3518
profiles that band was empty, which makes the value binary in disguise.
The upper pot now has a gradient and starts directly above `QuietMax`, with
its exponent chosen so the share above `TailFloor` stays exactly
`TailPercent`. Measured: 90.6 % quiet, 4.7 % between, 4.6 % from 40 up.

**Layer 4 is drawn jointly, not per column.** Roll the five parts
separately and you get impossible people — three hours at a stretch and yet
away every ten minutes. One roll picks a profile, the profile supplies all
five:

| Profile | Window starts | Block | Reliability | Interruptible |
|---|---|---|---|---|
| Evening | 19–22 | 120–240 min | high | rarely |
| Alongside | 8–21 | 20–75 min | low | constantly |
| Heavy | 12–18 | 76–200 min | middling | sometimes |
| Night | 22, 23, 0, 1, 2 | 76–210 min | high | rarely |

The night profile crosses midnight, and the overlap arithmetic handles it —
windows are compared modulo 24. Without it the server evening ended at
22:00 and not a single window began between 23:00 and 07:00, which for a
game whose real peak runs past midnight was the coarsest gap in the layer.

**Layer 3 is derived, never rolled.** Rolling it produces combinations that
do not exist — a questor whose gear is an *entry ticket to raiding*. It
follows from activity, career stage and guild membership, and is recomputed
at every stage change.

## Guilds grow from cliques

mod-playerbots creates guilds at server start and puts arbitrary bots in
them — level 1 characters that never met. This module replaces that.

A bot founds a guild only when all of this holds at once: enough **mutual**
bonds above `Found.MinBond` (a one-sided crush founds nothing), ambition
and sociability above their thresholds, a minimum level, no guild yet, and
the server-wide cooldown elapsed. Its founding circle are the bots it is
actually bonded to. Recruitment afterwards weighs the overlap of playing
windows, so a guild drifts towards one schedule the way a real one does.

**`AiPlayerbot.RandomBotGuildCount` must be `0`.** Both mechanisms at once
makes no sense. The module checks the value at every start and complains
loudly, and `apply-playerbots-patches.ps1` checks it before you build —
neither ever writes `playerbots.conf`; that file belongs to another module.

## Requirements

- **AzerothCore** master branch (WotLK 3.3.5a, build 12340)
- **mod-playerbots** ([liyunfan1223/mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)) — required, must be installed first
- MySQL/MariaDB with `acore_characters` database
- CMake 3.16+ and a C++17 compiler

**Tested on:**
- Windows Server 2022 + MSVC 2022
- Ubuntu 22.04 + GCC 11
- Debian 12 + Clang 15

## Installation

```bash
cd azerothcore-wotlk/modules
git clone <this-repo> mod-bot-social
```

Rebuild (CMake picks modules up automatically) and start worldserver once —
it creates the four tables in `acore_characters`.

Then set the master switch in `configs/modules/mod_bot_social.conf`:

```
BotSocial.Enable = 1
```

and run `.reload config` in the server console. No restart needed: the
module hooks `OnAfterConfigLoad` and re-reads **every** key.

> **Careful when updating:** a `.conf.dist` never overwrites an existing
> `.conf`. New keys from a newer release have to be copied into
> `configs/modules/mod_bot_social.conf` by hand — otherwise the code
> defaults apply silently.

## Tables

In `acore_characters`: `bot_social_bond`, `bot_social_profile`,
`bot_social_guild`, `bot_social_event`.

`bot_social_event` carries `map_id`/`zone_id` (0 = unknown) so an external
dashboard can ask *where* grudges and awards happen, and — with
`Trace.Enable` — a `level_up` row per level gained (weight = new level,
detail = old level). GM characters with `.gm on` are excluded from all
scoring and logging via `IgnoreGameMasters` (default on).

The SQL ships twice on purpose — under `base/` for a fresh database, under
`updates/` for one that is already populated. Changing a table means
touching both files.

> **Upgrading from 1.x: the migration is mandatory.** The six layers added
> fourteen columns to `bot_social_profile` and two to `bot_social_guild`.
> `data/sql/db-characters/updates/20260830_00_bot_social_layers.sql` runs
> through AzerothCore's UpdateFetcher on the next start; if you keep SQL
> updates switched off, apply it by hand. Without it the module cannot
> load its profiles.
>
> Existing profiles keep their bonds, reputation and activity. Their new
> columns start on the column defaults, and the module rolls the real
> values the next time each bot does something social — recognisable by
> `stage_since = 0`. Until then those rows are *not* representative, so
> leave them out of any statistic you build:
>
> ```sql
> SELECT COUNT(*) FROM bot_social_profile WHERE stage_since = 0;
> ```

## In-game commands

```
.social stats            Overview: bonds, friendships, grudges, guilds
.social bonds <name>     This character's strongest bonds
.social grudges <name>   The worst ones
.social who <name>       Full profile: all six layers, skill, reputation
```

## Configuration

Every key carries the `BotSocial.` prefix and is commented individually in
`conf/mod_bot_social.conf.dist`. The groups:

| Group | What it covers |
|---|---|
| Basics | `Enable`, `DebugLog`, `FlushInterval`, `AuthDatabaseName`, `BotAccountPrefix` |
| Gain | what creates affection: groups, minutes, dungeons, battlegrounds, resurrection |
| Conflict | what creates grudges: kicks, ninja loot, guild bank, ganking, duels |
| Human quirks | first impressions, archetype friction |
| Decay | how fast affection and grudges fade |
| Limits and consequences | Dunbar cap, friend list, avoidance, ignore list |
| The player | whether and how you take part in the graph |
| Reputation | server-wide standing from grudges and favours |
| Recruitment | how bots recruit for their guild |
| Ranks | promotions inside the guild |
| Schism | when a clique walks out together |
| Skill per bot | effect of `skill_tier` — **needs the patch below** |
| Layers | `Layers.Enable`, `Traits.*`, `Sadism.*` |
| Available time | `Time.Weight*` (four profiles), `Time.Minutes*`, `Time.Instance*` |
| Anchor | `Anchor.MinDungeons`, `Anchor.MinMinutes` |
| Founding | `Found.*` — thresholds, cooldown, guild cap |

All defaults are conservative: `Enable`, `Schism.Enable` and
`ReactDelay.Enable` ship as `0`.

Two settings are worth understanding rather than turning:

`Time.Weight*` are the four availability profiles, `40 / 35 / 13 / 12`.
They need not add up to 100 — they are weights, not percentages — but it
reads better if they do.

`FlushMaxStatements` (default `2000`) caps how many rows go into one write
transaction. `0` means no limit, and on a large server that is a real
hazard: after a mass migration half the population lands in a single
transaction and holds the character database while it runs.

## Optional: make skill matter (patch for mod-playerbots)

Every bot rolls a `skill_tier` from 1 to 5 on first contact — ten percent
are poor, eight percent very good, the rest spread in between. Without the
patch below that is just a number in the database that `.social who`
displays.

`patches/01-playerbots-reactdelay.patch` turns it into something you can
feel: reaction time. A poor player hesitates, a good one is quick — in
combat, while resting, in battlegrounds.

Run the installer from the module folder:

```powershell
.pply-playerbots-patches.ps1 -Check    # test only, changes nothing
.pply-playerbots-patches.ps1           # apply
.pply-playerbots-patches.ps1 -Revert   # undo
```

It finds `mod-playerbots` next to this module, applies every patch under
`patches/`, and is safe to run twice — an already-applied patch is detected
and skipped. Double-clicking `apply-playerbots-patches.bat` does the same.

By hand, if you prefer:

```bash
cd azerothcore-wotlk/modules/mod-playerbots
git apply ../mod-bot-social/patches/01-playerbots-reactdelay.patch
```

There is deliberately **no pre-patched copy of the file** to fall back on.
A copy would overwrite whatever mod-playerbots has changed since — silently,
including fixes you wanted. A patch that no longer fits stops and says so,
naming the file and the line, and that failure is the useful part.

Then in `mod_bot_social.conf`:

```
BotSocial.ReactDelay.Enable = 1
```

The patch is small and deliberately kept in one piece, so it can be put
back within minutes after a `git pull` in mod-playerbots. It adds three
includes, inserts a block in an anonymous namespace in front of
`GetReactDelay()`, and scales a single line in it — since all ten return
paths of that function derive from `base`, that takes effect everywhere.

mod-playerbots learns **nothing** about this module in the process: it just
reads the `bot_social_profile` table, once for all bots, refreshed every
`ReactDelay.RefreshSeconds`. With the module absent or the switch off, the
factor is 1.0 everywhere and behaviour is exactly as without the patch.

## Optional: fix a crash in mod-playerbots' guild validation

Independent of the recruiting feature above: mod-playerbots' hourly guild
cache validation (`PlayerbotGuildMgr::ValidateGuildCache`) dereferences the
guild leader's character-cache entry without checking it for null. If a
guild's leader character was deleted while the guild itself survived, that
lookup returns null and the next validation pass crashes the worldserver.

`patches/02-playerbots-guildcache-nullcheck.patch` adds the missing check:
a guild with no cached leader is skipped and logged instead of dereferenced.

Same two ways in as above:

```bash
cd azerothcore-wotlk/modules/mod-playerbots
git apply ../mod-bot-social/patches/02-playerbots-guildcache-nullcheck.patch
```

…or let `apply-playerbots-patches.ps1` handle both patches at once.

This one needs no config flag and no module coupling — it only guards a
null pointer that mod-playerbots itself already checks for the guild object
two lines above.

## Troubleshooting

### Module doesn't load / tables not created

**Symptom:** Worldserver starts but `.social stats` says "Unknown command"

**Solution:**
1. Check CMake output: `mod-bot-social` should appear in the modules list
2. Verify the module is in `modules/mod-bot-social/` (not a nested folder)
3. Rebuild: `cmake --build build --target clean && cmake --build build`
4. Check worldserver log for SQL errors during startup

### Bonds not forming / always zero

**Symptom:** `.social stats` shows zero bonds after hours of bot activity

**Solution:**
1. Verify `BotSocial.Enable = 1` in `configs/modules/mod_bot_social.conf`
2. Run `.reload config` in worldserver console
3. Check `BotSocial.Gain.*` values — if all zero, nothing creates affection
4. Verify bots are actually grouping: `.rndbot` should show bots in dungeons/raids

### Patch doesn't apply

**Symptom:** `git apply` fails with conflicts

**Solution:**
1. Check your mod-playerbots commit: `git log --oneline -1` in `modules/mod-playerbots/`
2. Look at the conflict: `git apply --3way patches/01-playerbots-reactdelay.patch` leaves conflict markers in the file
3. Merge by hand — the patch adds ~90 lines in three places (see the comments inside it), then regenerate it:
   `cmd /c "git -C <playerbots> diff -- src/Bot/PlayerbotAI.cpp > patches/01-playerbots-reactdelay.patch"`
   (use `cmd /c` for the redirect: PowerShell's own `>` writes UTF-16, which `git apply` cannot read)

### Config changes don't take effect

**Symptom:** Changed `mod_bot_social.conf` but behavior unchanged

**Solution:**
1. Run `.reload config` in worldserver console (no restart needed)
2. Check you edited `configs/modules/mod_bot_social.conf`, not the `.dist` file
3. Verify config syntax: no quotes around numbers, `=` not `:`, no trailing commas

### Performance issues with many bots

**Symptom:** Worldserver lag with 500+ bots

**Solution:**
1. Increase `BotSocial.FlushInterval` (default 600s) — less frequent DB writes
2. Raise `BotSocial.MaxBondsPerBot` only if needed (default 150 is Dunbar-optimal)
3. Disable `BotSocial.DebugLog` in production
4. Consider disabling `BotSocial.ReactDelay.Enable` if the patch causes issues

## Known Limits

- The triggers are timers, not events: recruitment, promotion and schism
  run on a clock and then look for candidates. That feels less like
  consequence than it should.
- The ringleader of a schism needs a hard-coded `ambition >= 60`; that is
  in no config file.
- Events land silently in `bot_social_event` — nobody in the game says
  anything about them.

## License

AGPL v3, same as AzerothCore.

---

# mod-bot-social — deutsche Fassung

Ein dauerhafter sozialer Graph für die Bots aus
[mod-playerbots](https://github.com/liyunfan1223/mod-playerbots). Die Bots
merken sich, mit wem sie gespielt haben, und handeln danach: sie schließen
Freundschaften, tragen einander etwas nach, werben für ihre Gilde an,
befördern und spalten sich.

Für AzerothCore, WotLK (3.3.5a).

## Der Gedanke dahinter

Das Modell bildet ein paar Dinge nach, die Menschen tatsächlich tun:

- **Zuneigung ist einseitig.** Pro Paar stehen zwei Zeilen in der Tabelle,
  mit unterschiedlichen Zahlen. A kann B mögen, während B von A nichts
  hält.
- **Groll verblasst langsamer als Zuneigung.** Negative Werte zerfallen nur
  ein Drittel so oft wie positive.
- **Die ersten Begegnungen zählen doppelt.** Ein erster Eindruck wiegt
  schwerer als der zwanzigste.
- **Niemand kennt achthundert Leute.** Eine Dunbar-Grenze kappt die
  schwächsten Bindungen — und bewahrt zugleich die Tabelle vor
  quadratischem Wachstum.
- **Wer anders spielen will, reibt sich.** Jeder Bot bekommt eine
  Tätigkeit (questor, dungeoneer, raider, pvper, gatherer, explorer,
  collector); unpassende Paarungen bekommen pro gemeinsamer Gruppe einen
  kleinen Abzug.

Der Graph liegt im Arbeitsspeicher, die Datenbank ist die Sicherung.

**Das Modul linkt nicht gegen mod-playerbots.** Es beobachtet nur
Kernereignisse und erkennt Bots über `WorldSession::IsBot()`. Ein Update
von mod-playerbots bricht es deshalb nicht.

## Die sechs Schichten (2.0)

Bis 1.x war ein Bot eine einzige Zahl: ein Archetyp, und alles andere folgte
daraus. Ein `casual` war automatisch weniger gesellig, ein `raider`
ehrgeiziger. Genau das gibt die Forschung nicht her — was jemand spielt,
sagt sehr wenig darüber, wie er ist. Zwei der sieben Archetypen, `casual`
und `loner`, waren überhaupt keine Tätigkeiten: das eine war ein Zeitplan,
das andere ein Wesenszug, beide als Tätigkeit verkleidet.

2.0 trennt das auf. Ein Bot besteht jetzt aus sechs unabhängigen Schichten:

| Schicht | Was sie ist | Woher sie kommt |
|---|---|---|
| 1 — Tätigkeit | questor, dungeoneer, raider, pvper, gatherer, explorer, collector | gewichteter Wurf |
| 2 — Wesen | Verträglichkeit, Ehrlichkeit, Gewissenhaftigkeit, Geselligkeit, Ehrgeiz, Sadismus | gewürfelt, unabhängig von Schicht 1 |
| 3 — Beweggrund für Ausrüstung | Mittel zum Zweck, die Zahl selbst, Zeichen nach außen, Eintrittskarte, Pflicht der Gilde | **abgeleitet**, nie gewürfelt |
| 4 — Verfügbarkeit | Fensterbeginn, Spanne, Blocklänge, Verlässlichkeit, Unterbrechbarkeit | **gemeinsam** gewürfelt |
| 5 — Verlauf | Anfang, Aufbau, Meisterschaft, Ausgebrannt, Rückkehr | folgt Ereignissen |
| 6 — Anker | der eine Mensch, wegen dem jemand einloggt | wird verdient, nie gewürfelt |

Drei davon haben eine Fußnote verdient, weil in jeder eine Falle steckt.

**Schicht 2 ist nicht eine einzige Glocke.** Vier Wesenszüge sind
normalverteilt um 50. Sadismus nicht: Volkmer und Kollegen (2023,
vorregistriert, N = 1026) zeigen über Quantilregression, dass Sadismus nur
die *oberen* Quantile der Trollneigung erklärt — für niedrige Werte sagt er
nichts. Deshalb liegt die Masse nahe null und ein schmaler Rand liegt hoch.
Die erste Umsetzung zog aus zwei getrennten Töpfen und ließ zwischen 16 und
39 gar nichts übrig; in einer lebenden Bevölkerung von 3518 Profilen war
dieses Band leer, was den Wert in Wahrheit binär macht. Der obere Topf hat
jetzt einen Verlauf und beginnt direkt über `QuietMax`, sein Exponent ist so
gewählt, dass der Anteil über `TailFloor` exakt `TailPercent` bleibt.
Gemessen: 90,6 % ruhig, 4,7 % dazwischen, 4,6 % ab 40.

**Schicht 4 wird gemeinsam gezogen, nicht spaltenweise.** Einzeln gewürfelt
entstehen unmögliche Menschen — drei Stunden am Stück und trotzdem alle zehn
Minuten weg. Ein Wurf wählt das Profil, das Profil liefert alle fünf Teile:

| Profil | Fenster beginnt | Block | Verlässlich | Unterbrechbar |
|---|---|---|---|---|
| Feierabend | 19–22 | 120–240 min | hoch | selten |
| Nebenher | 8–21 | 20–75 min | niedrig | ständig |
| Viel Zeit | 12–18 | 76–200 min | mittel | manchmal |
| Nachtschicht | 22, 23, 0, 1, 2 | 76–210 min | hoch | selten |

Die Nachtschicht läuft über Mitternacht, und die Deckungsrechnung kommt
damit zurecht — Fenster werden modulo 24 verglichen. Ohne sie endete der
Serverabend um 22 Uhr, und zwischen 23 und 7 begann kein einziges Fenster:
für ein Spiel, dessen echter Gipfel bis nach Mitternacht läuft, war das die
gröbste Lücke der ganzen Schicht.

**Schicht 3 wird abgeleitet, nie gewürfelt.** Würfeln erzeugt Kombinationen,
die es nicht gibt — einen Questgänger, dessen Ausrüstung eine
*Eintrittskarte ins Raiden* ist. Sie folgt aus Tätigkeit, Verlauf und
Gildenzugehörigkeit und wird bei jedem Stufenwechsel neu gesetzt.

## Gilden entstehen aus Cliquen

mod-playerbots legt beim Serverstart Gilden an und steckt beliebige Bots
hinein — Stufe-1-Charaktere, die sich nie begegnet sind. Dieses Modul
ersetzt das.

Ein Bot gründet erst, wenn alles zugleich zutrifft: genug **beidseitige**
Bindungen über `Found.MinBond` (eine einseitige Schwärmerei gründet nichts),
Ehrgeiz und Geselligkeit über ihren Schwellen, eine Mindeststufe, noch keine
Gilde, und die serverweite Wartezeit ist um. Der Gründungskreis sind die
Bots, mit denen er tatsächlich verbunden ist. Die Anwerbung danach gewichtet
die Überschneidung der Spielfenster — so driftet eine Gilde auf einen
gemeinsamen Zeitplan zu, wie eine echte auch.

**`AiPlayerbot.RandomBotGuildCount` muss `0` sein.** Beides zusammen ergibt
keinen Sinn. Das Modul prüft den Wert bei jedem Start und meldet sich laut,
und `apply-playerbots-patches.ps1` prüft ihn schon vor dem Bauen — geschrieben
wird `playerbots.conf` von beiden nie, die Datei gehört einem fremden Modul.

## Voraussetzungen

- **AzerothCore** master-Branch (WotLK 3.3.5a, Build 12340)
- **mod-playerbots** ([liyunfan1223/mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)) — erforderlich, muss zuerst installiert sein
- MySQL/MariaDB mit `acore_characters`-Datenbank
- CMake 3.16+ und ein C++17-Compiler

**Getestet auf:**
- Windows Server 2022 + MSVC 2022
- Ubuntu 22.04 + GCC 11
- Debian 12 + Clang 15

## Installation

```bash
cd azerothcore-wotlk/modules
git clone <dieses-repo> mod-bot-social
```

Dann neu bauen (CMake erfasst Module automatisch) und den worldserver
einmal starten — er legt die vier Tabellen in `acore_characters` an.

Danach in `configs/modules/mod_bot_social.conf` den Hauptschalter setzen:

```
BotSocial.Enable = 1
```

und `.reload config` in die Serverkonsole. Ein Neustart ist nicht nötig:
das Modul hängt an `OnAfterConfigLoad` und liest **alle** Schlüssel neu.

> **Achtung bei einem Update:** Eine `.conf.dist` überschreibt eine
> vorhandene `.conf` nie. Neue Schlüssel aus einer neueren Fassung muss man
> in `configs/modules/mod_bot_social.conf` von Hand nachtragen — sonst
> gelten stumm die Vorgaben aus dem Code.

## Tabellen

In `acore_characters`: `bot_social_bond`, `bot_social_profile`,
`bot_social_guild`, `bot_social_event`.

`bot_social_event` trägt `map_id`/`zone_id` (0 = unbekannt), damit ein
externes Dashboard fragen kann, *wo* Grolle und Vergaben entstehen, und —
mit `Trace.Enable` — eine `level_up`-Zeile pro Aufstieg (weight = neues
Level, detail = altes). GM-Charaktere mit `.gm on` sind über
`IgnoreGameMasters` (Standard an) von Punkten und Protokoll ausgenommen.

Das SQL liegt bewusst zweimal vor — unter `base/` für eine frische
Datenbank, unter `updates/` für eine bereits befüllte. Wer eine Tabelle
ändert, muss beide Dateien anfassen.

> **Umstieg von 1.x: die Wanderung ist Pflicht.** Die sechs Schichten haben
> `bot_social_profile` um vierzehn und `bot_social_guild` um zwei Spalten
> erweitert.
> `data/sql/db-characters/updates/20260830_00_bot_social_layers.sql` läuft
> beim nächsten Start über den UpdateFetcher von AzerothCore; wer
> SQL-Updates abgeschaltet hat, spielt sie von Hand ein. Ohne sie kann das
> Modul seine Profile nicht laden.
>
> Bestehende Profile behalten Bindungen, Ruf und Tätigkeit. Ihre neuen
> Spalten stehen zunächst auf den Spaltenvorgaben; die echten Werte würfelt
> das Modul, sobald der Bot das nächste Mal etwas Soziales tut — zu erkennen
> an `stage_since = 0`. Bis dahin sind diese Zeilen *nicht* repräsentativ
> und gehören aus jeder Statistik heraus:
>
> ```sql
> SELECT COUNT(*) FROM bot_social_profile WHERE stage_since = 0;
> ```

## Befehle im Spiel

```
.social stats            Überblick: Bindungen, Freundschaften, Grolle, Gilden
.social bonds <Name>     Die stärksten Bindungen dieses Charakters
.social grudges <Name>   Die schlechtesten
.social who <Name>       Volles Profil: alle sechs Schichten, Können, Ruf
```

## Konfiguration

Alle Schlüssel tragen das Präfix `BotSocial.` und sind in
`conf/mod_bot_social.conf.dist` einzeln kommentiert. Die Gruppen:

| Gruppe | Worum es geht |
|---|---|
| Grundlage | `Enable`, `DebugLog`, `FlushInterval`, `AuthDatabaseName`, `BotAccountPrefix` |
| Gewinn | was Zuneigung erzeugt: Gruppen, Minuten, Instanzen, Schlachtfelder, Wiederbelebung |
| Konflikt | was Groll erzeugt: Rauswurf, Ninja-Loot, Gildenbank, Gank, Duell |
| Menschliche Eigenheiten | erste Eindrücke, Archetyp-Reibung |
| Verblassen | wie schnell Zuneigung und Groll zerfallen |
| Grenzen und Folgen | Dunbar-Grenze, Freundesliste, Meiden, Ignorieren |
| Der Spieler | ob und wie du selbst im Graphen mitspielst |
| Ruf | serverweiter Leumund aus Grollen und Gefallen |
| Anwerbung | wie Bots für ihre Gilde werben |
| Ränge | Beförderungen innerhalb der Gilde |
| Gildenspaltung | wann eine Clique geschlossen austritt |
| Können pro Bot | Wirkung des `skill_tier` — **braucht den Patch unten** |
| Schichten | `Layers.Enable`, `Traits.*`, `Sadism.*` |
| Verfügbare Zeit | `Time.Weight*` (vier Profile), `Time.Minutes*`, `Time.Instance*` |
| Anker | `Anchor.MinDungeons`, `Anchor.MinMinutes` |
| Gründung | `Found.*` — Schwellen, Wartezeit, Gildenobergrenze |

Alle Vorgaben sind konservativ: `Enable`, `Schism.Enable` und
`ReactDelay.Enable` stehen ab Werk auf `0`.

Zwei Einstellungen sollte man eher verstehen als drehen:

`Time.Weight*` sind die vier Verfügbarkeitsprofile, `40 / 35 / 13 / 12`.
Sie müssen sich nicht zu 100 addieren — es sind Gewichte, keine Prozente —
aber es liest sich besser, wenn sie es tun.

`FlushMaxStatements` (Vorgabe `2000`) begrenzt, wie viele Zeilen in einer
Schreibtransaktion landen. `0` heißt keine Grenze, und auf einem großen
Server ist das eine echte Gefahr: nach einer Wanderung landet sonst die
halbe Bevölkerung in einer einzigen Transaktion und hält die
Charakterdatenbank fest, solange sie läuft.

## Optional: Können wirkt (Patch für mod-playerbots)

Jeder Bot würfelt beim ersten Kontakt ein `skill_tier` von 1 bis 5 — zehn
Prozent sind schlecht, acht Prozent sehr gut, der Rest verteilt sich
dazwischen. Ohne den folgenden Patch ist das nur eine Zahl in der
Datenbank, die `.social who` anzeigt.

`patches/01-playerbots-reactdelay.patch` macht daraus eine spürbare
Eigenschaft: die Reaktionszeit. Ein schlechter Spieler zögert, ein guter
ist flink — im Kampf, beim Rasten, im Schlachtfeld.

Das Installationsskript aus dem Modulordner starten:

```powershell
.pply-playerbots-patches.ps1 -Check    # nur prüfen, ändert nichts
.pply-playerbots-patches.ps1           # anwenden
.pply-playerbots-patches.ps1 -Revert   # zurücknehmen
```

Es findet `mod-playerbots` neben diesem Modul, wendet alle Patches aus
`patches/` an und darf zweimal laufen — ein bereits angewendeter Patch wird
erkannt und übersprungen. Ein Doppelklick auf
`apply-playerbots-patches.bat` tut dasselbe.

Von Hand, wer lieber selbst zusieht:

```bash
cd azerothcore-wotlk/modules/mod-playerbots
git apply ../mod-bot-social/patches/01-playerbots-reactdelay.patch
```

Eine **fertig geänderte Kopie der Datei liegt bewusst nicht bei**. Eine
Kopie überschreibt alles, was mod-playerbots seither an dieser Datei
geändert hat — stillschweigend, Fehlerbehebungen eingeschlossen. Ein Patch,
der nicht mehr passt, hält an und sagt es, mit Datei und Zeile. Genau dieses
Scheitern ist das Nützliche daran.

Dann in `mod_bot_social.conf`:

```
BotSocial.ReactDelay.Enable = 1
```

Der Patch ist klein und absichtlich zusammenhängend, damit er sich nach
einem `git pull` in mod-playerbots in Minuten wieder einsetzen lässt. Er
ändert drei Includes, fügt einen Block im anonymen Namensraum vor
`GetReactDelay()` ein und skaliert dort eine einzige Zeile — weil alle zehn
Rückgabepfade dieser Funktion aus `base` abgeleitet sind, wirkt das
überall.

mod-playerbots erfährt dabei **nichts** von diesem Modul: es liest nur die
Tabelle `bot_social_profile`, einmal für alle Bots, alle
`ReactDelay.RefreshSeconds` neu. Fehlt das Modul oder ist der Schalter aus,
ist der Faktor überall 1.0 und alles verhält sich wie ohne Patch.

## Optional: einen Absturz in der Gilden-Validierung von mod-playerbots beheben

Unabhängig von der Rekrutierungs-Funktion oben: mod-playerbots' stündliche
Gilden-Cache-Prüfung (`PlayerbotGuildMgr::ValidateGuildCache`) dereferenziert
den Character-Cache-Eintrag des Gildenmeisters ohne Nullprüfung. Wurde der
Charakter des Gildenmeisters gelöscht, während die Gilde selbst bestehen
blieb, liefert dieser Lookup null zurück und der nächste Prüflauf stürzt den
Worldserver ab.

`patches/02-playerbots-guildcache-nullcheck.patch` fügt die fehlende Prüfung
hinzu: eine Gilde ohne gecachten Gildenmeister wird übersprungen und
protokolliert, statt dereferenziert.

Dieselben zwei Wege wie oben:

```bash
cd azerothcore-wotlk/modules/mod-playerbots
git apply ../mod-bot-social/patches/02-playerbots-guildcache-nullcheck.patch
```

…oder `apply-playerbots-patches.ps1` beide Patches auf einmal erledigen
lassen.

Dieser Patch braucht keinen Konfig-Schalter und keine Modul-Kopplung — er
sichert nur einen Nullzeiger ab, den mod-playerbots für das Gildenobjekt
zwei Zeilen darüber bereits selbst prüft.

## Fehlerbehebung

### Modul lädt nicht / Tabellen fehlen

**Symptom:** Worldserver startet, aber `.social stats` sagt "Unknown command"

**Lösung:**
1. CMake-Ausgabe prüfen: `mod-bot-social` sollte in der Modulliste stehen
2. Pfad prüfen: Modul in `modules/mod-bot-social/` (nicht verschachtelt)
3. Neu bauen: `cmake --build build --target clean && cmake --build build`
4. Worldserver-Log auf SQL-Fehler beim Start prüfen

### Bindungen entstehen nicht / immer Null

**Symptom:** `.social stats` zeigt nach Stunden Bot-Aktivität null Bindungen

**Lösung:**
1. `BotSocial.Enable = 1` in `configs/modules/mod_bot_social.conf` prüfen
2. `.reload config` in der Worldserver-Konsole ausführen
3. `BotSocial.Gain.*`-Werte prüfen — wenn alle Null, entsteht keine Zuneigung
4. Prüfen, ob Bots tatsächlich in Gruppen sind: `.rndbot` sollte Bots in Instanzen zeigen

### Patch lässt sich nicht anwenden

**Symptom:** `git apply` schlägt mit Konflikten fehl

**Lösung:**
1. Commit von mod-playerbots prüfen: `git log --oneline -1` in `modules/mod-playerbots/`
2. Konflikt ansehen: `git apply --3way patches/01-playerbots-reactdelay.patch` hinterlässt Konfliktmarken in der Datei
3. Von Hand einpflegen — der Patch fügt ~90 Zeilen an drei Stellen ein (siehe die Kommentare darin), danach neu erzeugen:
   `cmd /c "git -C <playerbots> diff -- src/Bot/PlayerbotAI.cpp > patches/01-playerbots-reactdelay.patch"`
   (die Umleitung über `cmd /c`: PowerShells eigenes `>` schreibt UTF-16, und das kann `git apply` nicht lesen)

### Config-Änderungen wirken nicht

**Symptom:** `mod_bot_social.conf` geändert, aber Verhalten unverändert

**Lösung:**
1. `.reload config` in Worldserver-Konsole ausführen (kein Neustart nötig)
2. Prüfen, dass `configs/modules/mod_bot_social.conf` bearbeitet wurde, nicht die `.dist`-Datei
3. Config-Syntax prüfen: keine Anführungszeichen um Zahlen, `=` statt `:`, keine Kommata am Ende

### Performance-Probleme mit vielen Bots

**Symptom:** Worldserver-Lag mit 500+ Bots

**Lösung:**
1. `BotSocial.FlushInterval` erhöhen (Standard 600s) — seltener DB-Schreibvorgänge
2. `BotSocial.MaxBondsPerBot` nur bei Bedarf erhöhen (Standard 150 ist Dunbar-optimal)
3. `BotSocial.DebugLog` im Produktivbetrieb deaktivieren
4. `BotSocial.ReactDelay.Enable` bei Problemen mit dem Patch deaktivieren

## Bekannte Grenzen

- Die Auslöser sind Zeitgeber, keine Ereignisse: Anwerbung, Beförderung und
  Spaltung laufen im Takt und suchen sich dann ihre Kandidaten. Das wirkt
  weniger folgerichtig, als es sein müsste.
- Der Rädelsführer einer Spaltung braucht fest verdrahtet `ambition >= 60`;
  das steht in keiner Config.
- Die Ereignisse landen stumm in `bot_social_event` — im Spiel sagt
  niemand etwas dazu.


## Installation der Playerbots-Patches / Installing Playerbots Patches

**Deutsch:**

Dieses Modul liefert zwei Patches für `mod-playerbots`, die Kompatibilitätsprobleme beheben:

1. **01-playerbots-reactdelay.patch** — Macht `sRandomBotUpdateInterval` pro Bot konfigurierbar statt global (verhindert Konflikte mit Bot-Social-Rekrutierung).
2. **02-playerbots-guildcache-nullcheck.patch** — Fügt Null-Check für Gilden-Cache hinzu (verhindert Crash wenn Gilden-Leader gelöscht wird).

### Automatische Installation (empfohlen)

Führe eines der folgenden Skripte im Modul-Root aus:

- **Windows PowerShell:** `apply-playerbots-patches.ps1`
- **Windows CMD:** `apply-playerbots-patches.bat`

Die Skripte prüfen, ob `mod-playerbots` installiert ist, wenden die Patches via `git apply` an und sind idempotent (wiederholte Ausführung ist sicher). Bei Konflikten wird auf vorgepatchte Dateien zurückgegriffen.

### Manuelle Installation

Falls die Skripte nicht funktionieren:

1. Wechsle in dein `modules/mod-playerbots`-Verzeichnis
2. Führe für jeden Patch aus:
   ```bash
   git apply --whitespace=nowarn ../mod-bot-social/patches/01-playerbots-reactdelay.patch
   git apply --whitespace=nowarn ../mod-bot-social/patches/02-playerbots-guildcache-nullcheck.patch
   ```
3. Falls `git apply` fehlschlägt (z.B. wegen Upstream-Änderungen), kopiere die vorgepatchten Dateien aus `patches/playerbots/`:
   ```bash
   cp ../mod-bot-social/patches/playerbots/src/Bot/PlayerbotAI.cpp src/Bot/PlayerbotAI.cpp
   cp ../mod-bot-social/patches/playerbots/src/Mgr/Guild/PlayerbotGuildMgr.cpp src/Mgr/Guild/PlayerbotGuildMgr.cpp
   ```

---

**English:**

This module ships two patches for `mod-playerbots` that fix compatibility issues:

1. **01-playerbots-reactdelay.patch** — Makes `sRandomBotUpdateInterval` configurable per bot instead of global (prevents conflicts with Bot-Social recruitment).
2. **02-playerbots-guildcache-nullcheck.patch** — Adds null-check for guild cache (prevents crash when guild leader is deleted).

### Automatic Installation (recommended)

Run one of the following scripts in the module root:

- **Windows PowerShell:** `apply-playerbots-patches.ps1`
- **Windows CMD:** `apply-playerbots-patches.bat`

The scripts check if `mod-playerbots` is installed, apply the patches via `git apply`, and are idempotent (safe to run multiple times). On conflicts, they fall back to pre-patched files.

### Manual Installation

If the scripts don't work:

1. Change to your `modules/mod-playerbots` directory
2. Run for each patch:
   ```bash
   git apply --whitespace=nowarn ../mod-bot-social/patches/01-playerbots-reactdelay.patch
   git apply --whitespace=nowarn ../mod-bot-social/patches/02-playerbots-guildcache-nullcheck.patch
   ```
3. If `git apply` fails (e.g. due to upstream changes), copy the pre-patched files from `patches/playerbots/`:
   ```bash
   cp ../mod-bot-social/patches/playerbots/src/Bot/PlayerbotAI.cpp src/Bot/PlayerbotAI.cpp
   cp ../mod-bot-social/patches/playerbots/src/Mgr/Guild/PlayerbotGuildMgr.cpp src/Mgr/Guild/PlayerbotGuildMgr.cpp
   ```

## Lizenz

AGPL v3, wie AzerothCore.
