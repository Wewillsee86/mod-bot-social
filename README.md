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
  Every bot gets an archetype (questor, dungeoneer, raider, pvper,
  gatherer, casual, loner); clashing pairs take a small penalty per shared
  group.

The graph lives in memory; the database is the backup.

**The module does not link against mod-playerbots.** It only observes core
script hooks and recognises bots through `WorldSession::IsBot()`. An update
to mod-playerbots therefore cannot break it.

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

## In-game commands

```
.social stats            Overview: bonds, friendships, grudges, guilds
.social bonds <name>     This character's strongest bonds
.social grudges <name>   The worst ones
.social who <name>       Profile: archetype, skill, sociability, reputation
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

All defaults are conservative: `Enable`, `Schism.Enable` and
`ReactDelay.Enable` ship as `0`.

## Optional: make skill matter (patch for mod-playerbots)

Every bot rolls a `skill_tier` from 1 to 5 on first contact — ten percent
are poor, eight percent very good, the rest spread in between. Without the
patch below that is just a number in the database that `.social who`
displays.

`patches/01-playerbots-reactdelay.patch` turns it into something you can
feel: reaction time. A poor player hesitates, a good one is quick — in
combat, while resting, in battlegrounds.

Two ways in; either one is enough.

**Way 1, clean: apply the patch.** Still works when mod-playerbots has
moved on, as long as nobody touched the same function.

```bash
cd azerothcore-wotlk/modules/mod-playerbots
git apply ../mod-bot-social/patches/01-playerbots-reactdelay.patch
```

**Way 2, without git: copy the finished file.** Under `patches/` it sits in
the very subfolder it belongs in — the path is the instruction:

```
mod-bot-social/patches/mod-playerbots/src/Bot/PlayerbotAI.cpp
                       └────────────────────────────────────┘
                       copy exactly here, under modules/
```

So to `modules/mod-playerbots/src/Bot/PlayerbotAI.cpp`, replacing the
existing file.

> **Check first:** this file matches mod-playerbots `master` at commit
> `2f7d9f77`. On any other revision you would overwrite someone else's
> changes — use way 1 then. After copying, `git status` in mod-playerbots
> must show exactly one modified file; more than that means it was the
> wrong revision.

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

or copy `patches/mod-playerbots/src/Mgr/Guild/PlayerbotGuildMgr.cpp` to
`modules/mod-playerbots/src/Mgr/Guild/PlayerbotGuildMgr.cpp`, same commit
check as above (`2f7d9f77`) applies.

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
2. If not `2f7d9f77` or close: use the pre-patched file instead (copy from `patches/mod-playerbots/src/Bot/PlayerbotAI.cpp`)
3. Or manually merge: the patch adds ~80 lines in two places (see comments in patch file)

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
- **Wer anders spielen will, reibt sich.** Jeder Bot bekommt einen
  Archetyp (questor, dungeoneer, raider, pvper, gatherer, casual, loner);
  unpassende Paarungen bekommen pro gemeinsamer Gruppe einen kleinen Abzug.

Der Graph liegt im Arbeitsspeicher, die Datenbank ist die Sicherung.

**Das Modul linkt nicht gegen mod-playerbots.** Es beobachtet nur
Kernereignisse und erkennt Bots über `WorldSession::IsBot()`. Ein Update
von mod-playerbots bricht es deshalb nicht.

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

## Befehle im Spiel

```
.social stats            Überblick: Bindungen, Freundschaften, Grolle, Gilden
.social bonds <Name>     Die stärksten Bindungen dieses Charakters
.social grudges <Name>   Die schlechtesten
.social who <Name>       Profil: Archetyp, Können, Geselligkeit, Ruf
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

Alle Vorgaben sind konservativ: `Enable`, `Schism.Enable` und
`ReactDelay.Enable` stehen ab Werk auf `0`.

## Optional: Können wirkt (Patch für mod-playerbots)

Jeder Bot würfelt beim ersten Kontakt ein `skill_tier` von 1 bis 5 — zehn
Prozent sind schlecht, acht Prozent sehr gut, der Rest verteilt sich
dazwischen. Ohne den folgenden Patch ist das nur eine Zahl in der
Datenbank, die `.social who` anzeigt.

`patches/01-playerbots-reactdelay.patch` macht daraus eine spürbare
Eigenschaft: die Reaktionszeit. Ein schlechter Spieler zögert, ein guter
ist flink — im Kampf, beim Rasten, im Schlachtfeld.

Es liegen zwei Wege bei — beide führen zum selben Ergebnis, einer davon
reicht.

**Weg 1, sauber: den Patch anwenden.** Funktioniert auch dann noch, wenn
mod-playerbots inzwischen weitergezogen ist, solange niemand dieselbe
Funktion angefasst hat.

```bash
cd azerothcore-wotlk/modules/mod-playerbots
git apply ../mod-bot-social/patches/01-playerbots-reactdelay.patch
```

**Weg 2, ohne Git: die fertige Datei kopieren.** Unter `patches/` liegt sie
im selben Unterordner, in den sie gehört — der Pfad ist die Anleitung:

```
mod-bot-social/patches/mod-playerbots/src/Bot/PlayerbotAI.cpp
                       └────────────────────────────────────┘
                       genau hierhin kopieren, unter modules/
```

Also nach `modules/mod-playerbots/src/Bot/PlayerbotAI.cpp`, die vorhandene
Datei ersetzen.

> **Vorher prüfen:** Diese Datei entspricht mod-playerbots `master` beim
> Commit `2f7d9f77`. Wer eine andere Fassung hat, überschreibt damit
> fremde Änderungen — in dem Fall Weg 1 nehmen. Ein `git status` in
> mod-playerbots zeigt hinterher genau eine geänderte Datei; steht dort
> mehr, war es die falsche Fassung.

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

oder `patches/mod-playerbots/src/Mgr/Guild/PlayerbotGuildMgr.cpp` nach
`modules/mod-playerbots/src/Mgr/Guild/PlayerbotGuildMgr.cpp` kopieren,
gleicher Commit-Check wie oben (`2f7d9f77`).

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
2. Falls nicht `2f7d9f77` oder nah dran: fertige Datei stattdessen kopieren (aus `patches/mod-playerbots/src/Bot/PlayerbotAI.cpp`)
3. Oder manuell einpflegen: Patch fügt ~80 Zeilen an zwei Stellen ein (siehe Kommentare in Patch-Datei)

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
