# mod-temucraft-realm-first

Realm First! achievements at levels 60 and 70 for every class and race (plus the stock level 80 ones), and for the first to 300 / 375 / 450 skill in each profession, earned only by characters made fresh for the content tier they belong to. Random bots are kept out; players' own alt bots can be.

## Rules

A character earns a Realm First! the moment it reaches the level or skill (level-up, skill-up, trainer or quest skill), if it is the first to do so **and**:

1. **The tier is open.** Each achievement belongs to a tier: 0 = Vanilla (level 60, skill 300), 1 = TBC (70, 375), 2 = Wrath (80, 450). A tier opens on its `Temucraft.RealmFirst.Tier<N>.OpenDate`, or, without a date, when the realm's expansion reaches it (`Temucraft.RealmFirst.Expansion`, by default the same as `Temucraft.ZoneAccess.Expansion`). The moment the module first sees a tier open that way is kept in `mod_temucraft_realm_first_tier`.
2. **The character belongs to this realm.** It has a row in `mod_temucraft_realm_first_eligibility`, written when the character is created on this realm. Characters that already existed when the module first ran get one from a one-time start-up backfill (`ExistingCharactersEligible = 1`, the default), and a character that exists before a tier opens may earn that tier's firsts. With `ExistingCharactersEligible = 0` the stricter rule applies instead: no backfill, and only characters created at or after a tier's open time can earn its firsts.
3. **It is the same character.** Still on the account it was created on (`RequireSameAccount`), the same class, and, for race firsts, still the race it was created as (no race change).
4. **Not marked ineligible** by a GM (`.temucraft realmfirst mark <name> ineligible <reason>`).
5. **Never boosted by a GM command** towards that first (`MarkGmBoosted`). An AllCommandScript sees each command before it runs and, if the core resolves the typed words to one of these commands and lets the invoker run it (its own test, `GetAutoCompletionsFor`: abbreviations and the `command` table's level included), marks the character it acts on (a name in the command, else the selected player, else the GM): `.levelup`, `.character level`, `.quest complete`, `.quest reward` mark the level firsts; `.setskill <skill>`, `.learn <spell>` of a profession rank or recipe mark that profession; `.learn all crafts` / `.learn all recipes` mark every profession. It is permanent (`.temucraft realmfirst mark ... eligible` doesn't clear it) and is kept in `boost_levels`, `boost_skills` and `boost_reason`, which `.temucraft realmfirst eligible` shows. The core has no `.modify` subcommand that changes level or skill; `.maxskill` only raises level-bound skills (weapons, defense), not professions. Items given with `.additem` aren't tracked.
6. **Not a bot**: random bots (account name starting with `RNDBOT`) are out with `ExcludeRandomBots`; alt bots (a player's characters in the world without a game client) are out only with `ExcludeAltBots`.
7. **Not in GM mode**, and the account doesn't have RBAC 9 (*cannot earn realm first achievements*).

A player who reaches a free first but can't earn it is told why (`TellIneligible`), and the server log says so too.

### Copied, transferred and "continued" characters

Neither the core nor the Temucraft modules leave a trace when a character is copied: `.pdump load` writes the character's rows (with its old `creation_date`) and calls no script, a copied database row is invisible, and `.character changeaccount` only changes the account. So the module trusts only what it wrote itself: the eligibility row is made in `OnPlayerCreate`, which runs only when a player makes a character at the character screen.

- **pdump / copied rows / characters from another realm**, loaded after the module first ran: no row, never eligible (until a GM marks them eligible).
- **Loaded before the module first ran**: the backfill gave them a row like every other existing character. Mark them with `.temucraft realmfirst mark <name> ineligible <reason>`.
- **Moved to another account**: the row's account no longer matches, not eligible (`RequireSameAccount`).
- **Older characters**: eligible through the backfill (with `ExistingCharactersEligible = 0`: only for tiers that opened after they were created).
- **Random bots**: made by the bot factory, not at the character screen, so they have no row either.
- A character deleted for good loses its row (`OnPlayerDeleteFromDB`); one restored with `.character deleted restore` keeps it.

`.temucraft realmfirst mark <name> eligible` gives a character with no row one, using its `creation_date` from `characters`. The backfill runs once (`mod_temucraft_realm_first_state`, name `backfill`); delete that row to run it again at the next start (it only adds rows for characters that have none).

## The achievements

| IDs | What | Tier |
|---|---|---|
| 60000 | Realm First! Level 60 | 0 |
| 60001-60011 | Realm First! Level 60 *class* (60000 + class ID; 60010 unused) | 0 |
| 60021-60050 | Realm First! Level 60 *race* (60020 + race ID 1-30) | 0 |
| 60100, 60101-60111, 60121-60150 | the same at level 70 | 1 |
| 60200-60213 | Realm First! Artisan *profession* (300 skill) | 0 |
| 60250-60263 | Realm First! Master *profession* (375 skill) | 1 |
| 457-467, 1404-1413, 1429-1454, 1529-1543 | stock / WHE Realm First! Level 80 (overall, class, race) | 2 |
| 1414-1427 | stock Realm First! Grand Master *profession* (450) | 2 |

60000-60299 is reserved for this module. The new achievements are Feats of Strength (category 81), 0 points, flag 0x100 (realm first: the client shows its "Realm First!" message), and have **no criteria**: no stock code can ever complete them, only the module grants them. Races with two IDs (Pandaren, Dracthyr, Earthen, Haranir) have one per faction: 13, 25, 27 and 29 are Alliance and 14, 26, 28 and 30 Horde. That comes from the language each race has in Temucraft's ChrRaces.dbc (patch-5: Common for the first, Orcish for the second), not from retail, whose IDs differ (retail Earthen 85 is Alliance, so there the lower ID is Horde).

**Stock level 80 / 450 firsts** (`TakeOverStock`): with 1 (default) the module blocks their stock criteria (`CanCheckCriteria` / `IsCompletedCriteria` hooks) and grants them with the same rules (Wrath tier). Whoever already has one when the module first runs is imported as its holder. With 0 they are left to the core (anyone, bots included). Note: in the core's data only the stock 9 races have race data for their level 80 first; the WHE race ones (Goblin 1429 ... Haranir 1543) have none, so with `TakeOverStock = 0` the first character of any race to reach 80 gets all of them.

`tools/build_realm_first.py` makes the catalog SQL, the `achievement_dbc` SQL and the client DBC from its lists: edit it there and run it again.

## Commands

| Command | RBAC | |
|---|---|---|
| `.temucraft realmfirst list [held\|open\|all]` | 103901 (GM) | tiers and their open times, who holds what, what's still free |
| `.temucraft realmfirst eligible <name>` | 103902 (GM) | the character's row and, per tier, whether it may earn firsts and why not |
| `.temucraft realmfirst reset <achievement> [keep]` | 103903 (admin) | reopens one; the holder loses it (offline only) unless `keep` |
| `.temucraft realmfirst mark <name> eligible\|ineligible [reason]` | 103904 (admin) | for copied characters, or to vouch for one |
| `.temucraft realmfirst reload` | 103905 (admin) | re-reads the catalog, tiers and holders |

GM permissions are linked to roles 193 / 1012, admin ones to 192 / 1014.

## Install

1. Put this folder in `modules/`, re-run CMake and build.
2. The SQL in `data/sql/` is applied by the worldserver's module updater. The world files delete their own rows before inserting them; the characters tables are never emptied.
3. Copy `conf/mod-temucraft-realm-first.conf.dist` to `configs/modules/mod-temucraft-realm-first.conf` to change the settings.
4. **Client**: the new achievements must be in the client's `Achievement.dbc`, or they show as unknown. `client/patch-T.MPQ/DBFilesClient/Achievement.dbc` is the client's current file (from patch-5.MPQ, 1837 rows) plus the 110 new rows. `tools/merge_client.py <patch-T.MPQ> <new file>` writes a copy of patch-T with it added. The server reads the same rows from `achievement_dbc`, so it works before its DBCs are rebuilt.

## What it adds, and where

| Database | Table | |
|---|---|---|
| world | `mod_temucraft_realm_first` | the catalog: achievement, type, tier, level / class / race / skill, stock, enabled |
| world | `achievement_dbc` | 60000-60299 |
| world | `module_string` | 23 texts |
| characters | `mod_temucraft_realm_first_eligibility` | one row per character created here |
| characters | `mod_temucraft_realm_first_holder` | who holds each first (held = 0: reopened by a reset) |
| characters | `mod_temucraft_realm_first_tier` | when each tier opened |
| characters | `mod_temucraft_realm_first_state` | one-time backfill done |
| auth | `rbac_permissions` | 103901-103905 |

## Works with other modules

Doesn't need any other module. Reads `Temucraft.ZoneAccess.Expansion` from mod-temucraft-zone-access's config when `Temucraft.RealmFirst.Expansion` is -1. Recognises mod-playerbots' random bots by their account names (no code of mod-playerbots is used).

## Texts and translations

The 23 player-facing texts are in `module_string` under `mod-temucraft-realm-first`; translations go in `module_string_locale`. If a row is missing the English text in the code is used.

Part of the Temucraft modules for AzerothCore 3.3.5a.
