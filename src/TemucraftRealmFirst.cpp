/*
 * mod-temucraft-realm-first: Realm First! achievements at levels 60 and 70 (and, taking over the stock ones, 80) for
 * every class and race, and for each profession at 300 / 375 / 450 skill, earned only by characters made fresh for
 * the content tier they belong to.
 *
 * The catalog (mod_temucraft_realm_first, world DB) gives each achievement a type (overall level, class level, race
 * level, profession skill), a tier (0 Vanilla, 1 TBC, 2 Wrath...) and its goal. The module itself grants them, on
 * level-up and on skill-up, to the first character that:
 *   - belongs to an open tier: Temucraft.RealmFirst.Tier<N>.OpenDate has passed, or (no date) the realm's expansion
 *     (Temucraft.RealmFirst.Expansion, else Temucraft.ZoneAccess.Expansion) is at least N; the moment the module first
 *     saw a tier open is kept in mod_temucraft_realm_first_tier;
 *   - has a row in mod_temucraft_realm_first_eligibility (written when the character is created here; characters that
 *     existed when the module first ran get one from a one-time start-up backfill), not marked ineligible, still on
 *     the account and class it was made with, (race firsts) still its race, and never boosted towards that first by a
 *     GM command (.levelup, .character level, .quest complete/reward, .setskill, .learn of profession spells, .learn
 *     all crafts/recipes: seen by an AllCommandScript and recorded for good in the eligibility row);
 *   - with ExistingCharactersEligible = 0 only: created at or after the tier opened;
 *   - is not a random bot (account name prefix), nor an alt bot (a character with no client) if those are excluded;
 *   - is not in GM mode and its account may earn realm firsts (RBAC 9 not granted).
 * The new achievements (60000-60299) have no criteria, so no stock code can grant them. With TakeOverStock, the stock
 * level 80 / 450 skill Realm Firsts in the catalog are blocked from stock criteria and granted by the module instead.
 * Holders are in mod_temucraft_realm_first_holder (characters DB).
 *
 *   .temucraft realmfirst list [held|open|all]                 who holds what, and which tiers are open
 *   .temucraft realmfirst eligible <name>                      why a character can or can't earn them
 *   .temucraft realmfirst reset <achievement> [keep]           reopen one (keep: the holder keeps it)
 *   .temucraft realmfirst mark <name> eligible|ineligible [reason]
 *   .temucraft realmfirst reload                               re-read the catalog, tiers and holders
 */

#include "AccountMgr.h"
#include "AchievementMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "TemucraftModuleText.h"
#include "Timer.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    char const* const MODULE = "mod-temucraft-realm-first";

    enum RealmFirstType : uint8
    {
        TYPE_LEVEL      = 0,    // overall level
        TYPE_CLASS      = 1,    // level, one class
        TYPE_RACE       = 2,    // level, one race
        TYPE_PROFESSION = 3,    // skill value in one profession
        TYPE_MAX
    };

    constexpr uint32 MAX_TIERS = 10;
    constexpr uint32 MAX_ACHIEVEMENT_ID = 65536;    // character_achievement.achievement is a smallint unsigned

    struct Entry
    {
        uint32 achievement = 0;
        uint8 type = 0;
        uint8 tier = 0;
        uint8 level = 0;
        uint8 cls = 0;
        uint8 race = 0;
        uint16 skill = 0;
        uint16 skillValue = 0;
        bool stock = false;
        bool enabled = true;
        std::string comment;
    };

    struct Eligibility
    {
        uint32 account = 0;
        std::string name;
        uint8 race = 0;
        uint8 cls = 0;
        uint32 createdAt = 0;
        uint32 tiersAtCreation = 0;
        bool eligible = true;
        std::string source;
        std::string reason;
        std::string markedBy;
        uint32 markedAt = 0;
        bool boostLevels = false;
        std::string boostSkills;    // '' none, '*' every profession, else ",164,171,"
        std::string boostReason;
    };

    // Firsts a GM command boosted this character towards: never earned by it.
    struct Boost
    {
        bool levels = false;
        bool allSkills = false;
        std::set<uint32> skills;
        std::string reason;

        bool Any() const { return levels || allSkills || !skills.empty(); }
    };

    struct Holder
    {
        ObjectGuid::LowType guid = 0;
        std::string name;
        uint32 account = 0;
        uint32 earnedAt = 0;
        bool held = false;
        std::string source;
    };

    struct Settings
    {
        bool enable = true;
        int32 expansion = -1;
        bool excludeRandomBots = true;
        bool excludeAltBots = false;
        std::string randomBotPrefix = "RNDBOT";
        bool announce = true;
        bool tellIneligible = true;
        bool takeOverStock = true;
        bool sameAccount = true;
        bool existingEligible = true;
        bool markGmBoosts = true;
        std::array<bool, TYPE_MAX> types = { true, true, true, true };
        std::array<uint32, MAX_TIERS> openDate = { };   // Tier<N>.OpenDate, parsed (0 = none)
        int32 zoneAccessExpansion = 2;                  // Temucraft.ZoneAccess.Expansion, read with the config
    };

    Settings settings;

    // Everything below is read by map threads (level-ups and skill-ups happen there): one lock.
    std::mutex sLock;
    std::vector<Entry> sCatalog;
    std::unordered_map<uint32, size_t> sById;
    std::unordered_map<uint32, Holder> sHolders;
    std::array<uint32, MAX_TIERS> sStamped = { };   // mod_temucraft_realm_first_tier
    std::unordered_map<uint32, bool> sRandomBotAccount;
    std::unordered_map<ObjectGuid::LowType, Boost> sBoosts;   // marked since start-up (the row may not be written yet)

    // Read on every achievement criteria update by every player: lock-free flags.
    std::array<std::atomic<bool>, MAX_ACHIEVEMENT_ID> sManaged = { };   // the module decides who holds it
    std::array<std::atomic<bool>, MAX_ACHIEVEMENT_ID> sBlocked = { };   // stock criteria may not complete it

    thread_local bool tGranting = false;        // CompletedAchievement called by this module

    char const* T(ChatHandler* handler, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, handler, id, fallback); }
    char const* T(Player* player, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, player, id, fallback); }

    uint32 Now() { return uint32(GameTime::GetGameTime().count()); }

    std::string Date(uint32 t)
    {
        return t ? Acore::Time::TimeToTimestampStr(Seconds(t), "%Y-%m-%d %H:%M") : std::string("-");
    }

    // "YYYY-MM-DD HH:MM[:SS]" or "YYYY-MM-DD", server local time. 0 if empty or unreadable.
    uint32 ParseDate(std::string const& text)
    {
        if (text.empty())
            return 0;
        std::tm tm = { };
        std::istringstream in(text);
        in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (in.fail())
        {
            tm = { };
            std::istringstream in2(text);
            in2 >> std::get_time(&tm, "%Y-%m-%d %H:%M");
            if (in2.fail())
            {
                tm = { };
                std::istringstream in3(text);
                in3 >> std::get_time(&tm, "%Y-%m-%d");
                if (in3.fail())
                {
                    LOG_ERROR("module", "mod-temucraft-realm-first: can't read the date \"{}\" (use YYYY-MM-DD HH:MM:SS)", text);
                    return 0;
                }
            }
        }
        tm.tm_isdst = -1;
        time_t t = std::mktime(&tm);
        return t > 0 ? uint32(t) : 0;
    }

    int32 Expansion()
    {
        if (settings.expansion >= 0)
            return settings.expansion;
        return settings.zoneAccessExpansion;
    }

    // When the tier opened (0 = not open). A configured date wins; otherwise the expansion decides and the time the
    // module first saw it open is used.
    uint32 TierOpenTime(uint8 tier)
    {
        if (tier >= MAX_TIERS)
            return 0;
        if (settings.openDate[tier])
            return settings.openDate[tier];
        if (int32(tier) > Expansion())
            return 0;
        std::lock_guard<std::mutex> guard(sLock);
        return sStamped[tier];
    }

    bool TierOpen(uint8 tier, uint32* openTime = nullptr)
    {
        uint32 t = TierOpenTime(tier);
        if (openTime)
            *openTime = t;
        return t && Now() >= t;
    }

    std::string TierName(uint8 tier)
    {
        static char const* const names[] = { "Vanilla", "The Burning Crusade", "Wrath of the Lich King", "Cataclysm",
            "Mists of Pandaria" };
        return tier < std::size(names) ? names[tier] : Acore::StringFormat("tier {}", tier);
    }

    std::string AchievementName(uint32 id)
    {
        if (AchievementEntry const* entry = sAchievementStore.LookupEntry(id))
            if (entry->name[0] && *entry->name[0])
                return entry->name[0];
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sById.find(id);
        return itr != sById.end() ? sCatalog[itr->second].comment : Acore::StringFormat("achievement {}", id);
    }

    std::string Escape(std::string text)
    {
        CharacterDatabase.EscapeString(text);
        return text;
    }

    // ---------------------------------------------------------------- bots

    bool IsRandomBotAccount(uint32 account)
    {
        if (settings.randomBotPrefix.empty())
            return false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto itr = sRandomBotAccount.find(account);
            if (itr != sRandomBotAccount.end())
                return itr->second;
        }
        std::string name;
        bool bot = false;
        if (AccountMgr::GetName(account, name) && name.size() >= settings.randomBotPrefix.size())
            bot = std::equal(settings.randomBotPrefix.begin(), settings.randomBotPrefix.end(), name.begin(),
                [](char a, char b) { return std::toupper(uint8(a)) == std::toupper(uint8(b)); });
        std::lock_guard<std::mutex> guard(sLock);
        sRandomBotAccount[account] = bot;
        return bot;
    }

    enum BotKind { BOT_NONE, BOT_RANDOM, BOT_ALT };

    // Random bots: accounts whose name starts with RandomBotAccountPrefix (mod-playerbots' RNDBOT accounts).
    // Alt bots: any other character that is in the world without a game client (a player's own bots).
    BotKind GetBotKind(Player* player)
    {
        WorldSession* session = player->GetSession();
        if (session && IsRandomBotAccount(session->GetAccountId()))
            return BOT_RANDOM;
        if (!session || session->IsSocketClosed())
            return BOT_ALT;
        return BOT_NONE;
    }

    // ---------------------------------------------------------------- loading

    void LoadCatalog()
    {
        std::vector<Entry> catalog;
        if (QueryResult result = WorldDatabase.Query("SELECT `Achievement`, `Type`, `Tier`, `Level`, `Class`, `Race`, `Skill`, "
            "`SkillValue`, `Stock`, `Enabled`, `Comment` FROM `mod_temucraft_realm_first`"))
        {
            do
            {
                Field* f = result->Fetch();
                Entry e;
                e.achievement = f[0].Get<uint32>();
                e.type = f[1].Get<uint8>();
                e.tier = f[2].Get<uint8>();
                e.level = f[3].Get<uint8>();
                e.cls = f[4].Get<uint8>();
                e.race = f[5].Get<uint8>();
                e.skill = f[6].Get<uint16>();
                e.skillValue = f[7].Get<uint16>();
                e.stock = f[8].Get<uint8>() != 0;
                e.enabled = f[9].Get<uint8>() != 0;
                e.comment = f[10].Get<std::string>();
                if (e.achievement >= MAX_ACHIEVEMENT_ID || e.type >= TYPE_MAX || e.tier >= MAX_TIERS)
                {
                    LOG_ERROR("module", "mod-temucraft-realm-first: catalog row {} skipped (achievement < 65536, type 0-3, tier 0-9)", e.achievement);
                    continue;
                }
                if (!sAchievementStore.LookupEntry(e.achievement))
                {
                    LOG_ERROR("module", "mod-temucraft-realm-first: achievement {} ({}) is not in Achievement.dbc nor achievement_dbc: skipped",
                        e.achievement, e.comment);
                    continue;
                }
                catalog.push_back(std::move(e));
            } while (result->NextRow());
        }

        std::lock_guard<std::mutex> guard(sLock);
        for (Entry const& e : sCatalog)
        {
            sManaged[e.achievement].store(false);
            sBlocked[e.achievement].store(false);
        }
        sCatalog = std::move(catalog);
        sById.clear();
        for (size_t i = 0; i < sCatalog.size(); ++i)
        {
            Entry const& e = sCatalog[i];
            sById[e.achievement] = i;
            // the module decides an entry when it's on; stock entries only when taken over
            bool managed = settings.enable && e.enabled && settings.types[e.type] && (!e.stock || settings.takeOverStock);
            sManaged[e.achievement].store(managed);
            sBlocked[e.achievement].store(managed && e.stock);
        }
    }

    void LoadTiers()
    {
        std::array<uint32, MAX_TIERS> stamped = { };
        if (QueryResult result = CharacterDatabase.Query("SELECT `tier`, `opened_at` FROM `mod_temucraft_realm_first_tier`"))
        {
            do
            {
                Field* f = result->Fetch();
                uint8 tier = f[0].Get<uint8>();
                if (tier < MAX_TIERS)
                    stamped[tier] = f[1].Get<uint32>();
            } while (result->NextRow());
        }
        // a tier opened by the expansion (no date in the config) is stamped the first time the module sees it open
        if (settings.enable)
        {
            int32 expansion = Expansion();
            for (uint8 tier = 0; tier < MAX_TIERS && int32(tier) <= expansion; ++tier)
            {
                if (stamped[tier] || settings.openDate[tier])
                    continue;
                stamped[tier] = Now();
                CharacterDatabase.Execute("INSERT IGNORE INTO `mod_temucraft_realm_first_tier` (`tier`, `opened_at`) VALUES ({}, {})",
                    tier, stamped[tier]);
                LOG_INFO("module", "mod-temucraft-realm-first: tier {} ({}) is open from now on: only characters created from now "
                    "on can earn its Realm Firsts", tier, TierName(tier));
            }
        }
        std::lock_guard<std::mutex> guard(sLock);
        sStamped = stamped;
    }

    void LoadHolders()
    {
        std::unordered_map<uint32, Holder> holders;
        if (QueryResult result = CharacterDatabase.Query("SELECT `achievement`, `guid`, `name`, `account`, `earned_at`, `held`, `source` "
            "FROM `mod_temucraft_realm_first_holder`"))
        {
            do
            {
                Field* f = result->Fetch();
                Holder h;
                h.guid = f[1].Get<uint32>();
                h.name = f[2].Get<std::string>();
                h.account = f[3].Get<uint32>();
                h.earnedAt = f[4].Get<uint32>();
                h.held = f[5].Get<uint8>() != 0;
                h.source = f[6].Get<std::string>();
                holders[f[0].Get<uint32>()] = std::move(h);
            } while (result->NextRow());
        }

        // Achievements the module decides that someone already has (stock firsts earned before the module, or before
        // TakeOverStock was turned on) and that have no holder row yet: the earliest one is the holder.
        std::vector<uint32> missing;
        {
            std::lock_guard<std::mutex> guard(sLock);
            for (Entry const& e : sCatalog)
                if (sManaged[e.achievement].load() && !holders.count(e.achievement))
                    missing.push_back(e.achievement);
        }
        if (!missing.empty())
        {
            std::string ids;
            for (uint32 id : missing)
                ids += (ids.empty() ? "" : ",") + std::to_string(id);
            if (QueryResult result = CharacterDatabase.Query("SELECT ca.`achievement`, ca.`guid`, ca.`date`, c.`name`, c.`account` "
                "FROM `character_achievement` ca JOIN `characters` c ON c.`guid` = ca.`guid` WHERE ca.`achievement` IN ({}) "
                "ORDER BY ca.`date`, ca.`guid`", ids))
            {
                do
                {
                    Field* f = result->Fetch();
                    uint32 id = f[0].Get<uint32>();
                    if (holders.count(id))
                        continue;   // a later earner of the same one
                    Holder h;
                    h.guid = f[1].Get<uint32>();
                    h.earnedAt = f[2].Get<uint32>();
                    h.name = f[3].Get<std::string>();
                    h.account = f[4].Get<uint32>();
                    h.held = true;
                    h.source = "imported";
                    CharacterDatabase.Execute("INSERT IGNORE INTO `mod_temucraft_realm_first_holder` (`achievement`, `guid`, `name`, "
                        "`account`, `earned_at`, `held`, `source`) VALUES ({}, {}, '{}', {}, {}, 1, 'imported')", id, h.guid,
                        Escape(h.name), h.account, h.earnedAt);
                    LOG_INFO("module", "mod-temucraft-realm-first: {} already held by {} (imported)", id, h.name);
                    holders[id] = std::move(h);
                } while (result->NextRow());
            }
        }

        std::lock_guard<std::mutex> guard(sLock);
        sHolders = std::move(holders);
    }

    // Characters that existed when the module first ran (ExistingCharactersEligible): one row each, once. Later
    // characters get theirs at creation, so a character that arrives later by pdump or copy still has none.
    void BackfillExisting()
    {
        if (!settings.enable || !settings.existingEligible)
            return;
        if (CharacterDatabase.Query("SELECT 1 FROM `mod_temucraft_realm_first_state` WHERE `name` = 'backfill'"))
            return;
        uint32 tiers = 0;
        for (uint8 tier = 0; tier < MAX_TIERS; ++tier)
            if (TierOpen(tier))
                tiers |= 1u << tier;
        CharacterDatabase.DirectExecute("INSERT IGNORE INTO `mod_temucraft_realm_first_eligibility` (`guid`, `account`, `name`, `race`, "
            "`class`, `created_at`, `tiers_at_creation`, `eligible`, `source`, `reason`) SELECT `guid`, `account`, `name`, `race`, `class`, "
            "UNIX_TIMESTAMP(`creation_date`), {}, 1, 'existing', 'existed when the module first ran' FROM `characters`", tiers);
        CharacterDatabase.DirectExecute("INSERT IGNORE INTO `mod_temucraft_realm_first_state` (`name`, `value`) VALUES ('backfill', {})", Now());
        LOG_INFO("module", "mod-temucraft-realm-first: every existing character got an eligibility row (one-time backfill)");
    }

    void LoadAll()
    {
        LoadCatalog();
        LoadTiers();
        BackfillExisting();
        LoadHolders();
        std::lock_guard<std::mutex> guard(sLock);
        uint32 managed = 0;
        for (Entry const& e : sCatalog)
            managed += sManaged[e.achievement].load() ? 1 : 0;
        LOG_INFO("server.loading", ">> mod-temucraft-realm-first: {} catalog rows, {} decided by the module, {} holders",
            sCatalog.size(), managed, sHolders.size());
    }

    Optional<Eligibility> LoadEligibility(ObjectGuid::LowType guid)
    {
        QueryResult result = CharacterDatabase.Query("SELECT `account`, `name`, `race`, `class`, `created_at`, `tiers_at_creation`, "
            "`eligible`, `source`, `reason`, `marked_by`, `marked_at`, `boost_levels`, `boost_skills`, `boost_reason` "
            "FROM `mod_temucraft_realm_first_eligibility` WHERE `guid` = {}", guid);
        if (!result)
            return std::nullopt;
        Field* f = result->Fetch();
        Eligibility e;
        e.account = f[0].Get<uint32>();
        e.name = f[1].Get<std::string>();
        e.race = f[2].Get<uint8>();
        e.cls = f[3].Get<uint8>();
        e.createdAt = f[4].Get<uint32>();
        e.tiersAtCreation = f[5].Get<uint32>();
        e.eligible = f[6].Get<uint8>() != 0;
        e.source = f[7].Get<std::string>();
        e.reason = f[8].Get<std::string>();
        e.markedBy = f[9].Get<std::string>();
        e.markedAt = f[10].Get<uint32>();
        e.boostLevels = f[11].Get<uint8>() != 0;
        e.boostSkills = f[12].Get<std::string>();
        e.boostReason = f[13].Get<std::string>();
        return e;
    }

    // A random bot has no eligibility row (the bot factory doesn't create characters the normal way). When random
    // bots are allowed, one is made up from the characters table: its creation date and account.
    Optional<Eligibility> LoadFromCharacters(ObjectGuid::LowType guid)
    {
        QueryResult result = CharacterDatabase.Query("SELECT `account`, `name`, `race`, `class`, UNIX_TIMESTAMP(`creation_date`) "
            "FROM `characters` WHERE `guid` = {}", guid);
        if (!result)
            return std::nullopt;
        Field* f = result->Fetch();
        Eligibility e;
        e.account = f[0].Get<uint32>();
        e.name = f[1].Get<std::string>();
        e.race = f[2].Get<uint8>();
        e.cls = f[3].Get<uint8>();
        e.createdAt = f[4].Get<uint32>();
        e.source = "random bot";
        return e;
    }

    // ---------------------------------------------------------------- GM boosts

    Boost BoostOf(ObjectGuid::LowType guid, Optional<Eligibility> const& row)
    {
        Boost b;
        if (row)
        {
            b.levels = row->boostLevels;
            b.allSkills = row->boostSkills == "*";
            for (std::string_view part : Acore::Tokenize(row->boostSkills, ',', false))
                if (Optional<uint32> skill = Acore::StringTo<uint32>(part))
                    b.skills.insert(*skill);
            b.reason = row->boostReason;
        }
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sBoosts.find(guid);
        if (itr != sBoosts.end())
        {
            b.levels |= itr->second.levels;
            b.allSkills |= itr->second.allSkills;
            b.skills.insert(itr->second.skills.begin(), itr->second.skills.end());
            if (!itr->second.reason.empty())
                b.reason = itr->second.reason;
        }
        return b;
    }

    std::string SkillsText(Boost const& b)
    {
        if (b.allSkills)
            return "*";
        std::string out;
        for (uint32 skill : b.skills)
            out += Acore::StringFormat(",{}", skill);
        return out.empty() ? out : out + ",";
    }

    std::string BoostVerdict(Boost const& b, Entry const& e)
    {
        bool hit = e.type == TYPE_PROFESSION ? (b.allSkills || b.skills.count(e.skill)) : b.levels;
        return hit ? "boosted towards it by a GM command (" + b.reason + ")" : "";
    }

    // ---------------------------------------------------------------- the rules

    // Whether this character (as it is now) may earn firsts of this tier, ignoring bots and GM rules (they need the
    // character online). The reason is empty when it may.
    std::string CharacterVerdict(Optional<Eligibility> const& row, uint32 account, uint8 race, uint8 cls, uint8 tier, uint8 raceOfEntry,
        Boost const* boost = nullptr, Entry const* entry = nullptr)
    {
        if (!row)
            return "no eligibility row: not created on this realm while the module ran (pdump, copy, older character, bot)";
        if (!row->eligible)
            return Acore::StringFormat("marked ineligible by {} on {}{}{}", row->markedBy.empty() ? "a GM" : row->markedBy,
                Date(row->markedAt), row->reason.empty() ? "" : ": ", row->reason);
        if (settings.sameAccount && row->account != account)
            return Acore::StringFormat("moved from account {} to account {}", row->account, account);
        if (row->cls && cls && row->cls != cls)
            return Acore::StringFormat("not the character the row was made for (class {}, now {})", row->cls, cls);
        uint32 openTime = 0;
        if (!TierOpen(tier, &openTime))
            return openTime ? Acore::StringFormat("{} opens on {}", TierName(tier), Date(openTime))
                            : Acore::StringFormat("{} is not open", TierName(tier));
        if (boost && entry)
            if (std::string why = BoostVerdict(*boost, *entry); !why.empty())
                return why;
        if (!settings.existingEligible && row->createdAt < openTime)
            return Acore::StringFormat("created {}, before {} opened ({})", Date(row->createdAt), TierName(tier), Date(openTime));
        if (raceOfEntry && race != row->race)
            return Acore::StringFormat("race changed since creation (was {})", row->race);
        return "";
    }

    std::string BotVerdict(Player* player)
    {
        switch (GetBotKind(player))
        {
            case BOT_RANDOM: return settings.excludeRandomBots ? "random bot" : "";
            case BOT_ALT:    return settings.excludeAltBots ? "played by a bot (no game client)" : "";
            default:         return "";
        }
    }

    bool Held(uint32 achievement, Holder* holder = nullptr)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sHolders.find(achievement);
        if (itr == sHolders.end() || !itr->second.held)
            return false;
        if (holder)
            *holder = itr->second;
        return true;
    }

    void Announce(Player* player, uint32 achievement)
    {
        if (!settings.announce)
            return;
        std::string text = Acore::StringFormat(T(static_cast<Player*>(nullptr), 1, "|cffffd100[Realm First!]|r {} has earned {}!"),
            player->GetName(), AchievementName(achievement));
        ChatHandler(nullptr).SendWorldText(text);
    }

    // The entries this character just reached and that are still free. Copied, so the lock isn't held while granting.
    template <typename Match>
    std::vector<Entry> FreeEntries(Match match)
    {
        std::vector<Entry> out;
        std::lock_guard<std::mutex> guard(sLock);
        for (Entry const& e : sCatalog)
        {
            if (!sManaged[e.achievement].load() || !match(e))
                continue;
            auto itr = sHolders.find(e.achievement);
            if (itr != sHolders.end() && itr->second.held)
                continue;
            out.push_back(e);
        }
        return out;
    }

    void TryGrant(Player* player, std::vector<Entry> const& entries)
    {
        if (entries.empty() || !settings.enable)
            return;

        // checked once for all of them; bots are turned away before any database query
        WorldSession* session = player->GetSession();
        if (std::string botWhy = BotVerdict(player); !botWhy.empty())
        {
            LOG_DEBUG("module", "mod-temucraft-realm-first: {} reached a free realm first: {}, excluded", player->GetName(), botWhy);
            return;
        }
        bool rbacDenied = session && session->HasPermission(rbac::RBAC_PERM_CANNOT_EARN_REALM_FIRST_ACHIEVEMENTS);
        Optional<Eligibility> row = LoadEligibility(player->GetGUID().GetCounter());
        if (!row && GetBotKind(player) == BOT_RANDOM)
            row = LoadFromCharacters(player->GetGUID().GetCounter());
        bool quiet = GetBotKind(player) != BOT_NONE;
        Boost boost = BoostOf(player->GetGUID().GetCounter(), row);
        uint32 account = session ? session->GetAccountId() : 0;

        for (Entry const& e : entries)
        {
            AchievementEntry const* achievement = sAchievementStore.LookupEntry(e.achievement);
            if (!achievement || player->HasAchieved(e.achievement))
                continue;

            std::string why;
            if (player->IsGameMaster())
                why = "GM mode is on";
            if (why.empty() && rbacDenied)
                why = "the account may not earn realm firsts (RBAC 9)";
            if (why.empty())
                why = CharacterVerdict(row, account, player->getRace(), player->getClass(), e.tier, e.type == TYPE_RACE ? e.race : 0,
                    &boost, &e);
            if (!why.empty())
            {
                LOG_INFO("module", "mod-temucraft-realm-first: {} reached {} first but is not eligible: {}", player->GetName(),
                    e.achievement, why);
                if (settings.tellIneligible && !quiet)
                    ChatHandler(session).PSendSysMessage(T(player, 2, "|cffffd100[Realm First!]|r You reached {} first, but this "
                        "character can't earn it: {}."), AchievementName(e.achievement), why);
                continue;
            }

            // reserve it, so two characters reaching it at once on different map threads can't both get it
            {
                std::lock_guard<std::mutex> guard(sLock);
                Holder& h = sHolders[e.achievement];
                if (h.held)
                    continue;
                h.guid = player->GetGUID().GetCounter();
                h.name = player->GetName();
                h.account = account;
                h.earnedAt = Now();
                h.held = true;
                h.source = "module";
            }

            tGranting = true;
            player->CompletedAchievement(achievement);
            tGranting = false;

            if (!player->HasAchieved(e.achievement))
            {
                // refused by the core or another script (e.g. RBAC "cannot earn achievements"): free it again
                std::lock_guard<std::mutex> guard(sLock);
                sHolders[e.achievement].held = false;
                LOG_INFO("module", "mod-temucraft-realm-first: {} could not be given {} (refused by the core or a script)",
                    player->GetName(), e.achievement);
                continue;
            }

            CharacterDatabase.Execute("REPLACE INTO `mod_temucraft_realm_first_holder` (`achievement`, `guid`, `name`, `account`, "
                "`earned_at`, `held`, `source`, `note`) VALUES ({}, {}, '{}', {}, {}, 1, 'module', '')", e.achievement,
                player->GetGUID().GetCounter(), Escape(player->GetName()), account, Now());
            LOG_INFO("module", "mod-temucraft-realm-first: {} (account {}) earned {} {}", player->GetName(), account,
                e.achievement, AchievementName(e.achievement));
            Announce(player, e.achievement);
        }
    }

    // ---------------------------------------------------------------- commands' helpers

    struct Target
    {
        ObjectGuid guid;
        std::string name;
        uint32 account = 0;
        uint8 race = 0;
        uint8 cls = 0;
        uint8 level = 0;
        Player* online = nullptr;
    };

    Optional<Target> FindTarget(std::string const& name)
    {
        std::string normal = name;
        if (!normalizePlayerName(normal))
            return std::nullopt;
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByName(normal);
        if (!cache)
            return std::nullopt;
        Target t;
        t.guid = cache->Guid;
        t.name = cache->Name;
        t.account = cache->AccountId;
        t.race = cache->Race;
        t.cls = cache->Class;
        t.level = cache->Level;
        t.online = ObjectAccessor::FindConnectedPlayer(cache->Guid);
        if (t.online)
        {
            t.race = t.online->getRace();
            t.level = t.online->GetLevel();
        }
        return t;
    }

    std::string ByName(ChatHandler* handler)
    {
        if (WorldSession* session = handler->GetSession())
            return session->GetPlayerName();
        return "console";
    }
}

class TemucraftRealmFirst_Player : public PlayerScript
{
public:
    TemucraftRealmFirst_Player() : PlayerScript("TemucraftRealmFirst_Player", { PLAYERHOOK_ON_CREATE, PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_UPDATE_SKILL, PLAYERHOOK_ON_SET_SKILL, PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_ON_ACHI_COMPLETE }) { }

    // The eligibility row: made only here, so characters that arrive any other way (pdump, a copied database row,
    // the random bot factory) have none and can't earn realm firsts.
    void OnPlayerCreate(Player* player) override
    {
        WorldSession* session = player->GetSession();
        uint32 account = session ? session->GetAccountId() : 0;
        uint32 now = Now();
        uint32 tiers = 0;
        for (uint8 tier = 0; tier < MAX_TIERS; ++tier)
            if (TierOpen(tier))
                tiers |= 1u << tier;
        CharacterDatabase.Execute("INSERT IGNORE INTO `mod_temucraft_realm_first_eligibility` (`guid`, `account`, `name`, `race`, "
            "`class`, `created_at`, `tiers_at_creation`, `eligible`, `source`) VALUES ({}, {}, '{}', {}, {}, {}, {}, 1, 'created')",
            player->GetGUID().GetCounter(), account, Escape(player->GetName()), player->getRace(), player->getClass(), now, tiers);
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_temucraft_realm_first_eligibility` WHERE `guid` = {}", guid);
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!settings.enable)
            return;
        uint8 level = player->GetLevel();
        if (level <= oldLevel)
            return;
        uint8 race = player->getRace(), cls = player->getClass();
        TryGrant(player, FreeEntries([&](Entry const& e)
        {
            if (e.type == TYPE_PROFESSION || e.level <= oldLevel || e.level > level)
                return false;
            return e.type == TYPE_LEVEL || (e.type == TYPE_CLASS && e.cls == cls) || (e.type == TYPE_RACE && e.race == race);
        }));
    }

    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 value, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
    {
        Skill(player, skillId, value, newValue);
    }

    void OnPlayerSetSkill(Player* player, uint32 skillId, uint32 value, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
    {
        Skill(player, skillId, value, newValue);
    }

    // Given some other way (.achievement add): recorded as the holder if there's none.
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if (tGranting || !achievement || achievement->ID >= MAX_ACHIEVEMENT_ID || !sManaged[achievement->ID].load())
            return;
        std::lock_guard<std::mutex> guard(sLock);
        Holder& h = sHolders[achievement->ID];
        if (h.held)
            return;
        h.guid = player->GetGUID().GetCounter();
        h.name = player->GetName();
        h.account = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
        h.earnedAt = Now();
        h.held = true;
        h.source = "other";
        CharacterDatabase.Execute("REPLACE INTO `mod_temucraft_realm_first_holder` (`achievement`, `guid`, `name`, `account`, "
            "`earned_at`, `held`, `source`, `note`) VALUES ({}, {}, '{}', {}, {}, 1, 'other', 'not granted by the module')",
            achievement->ID, h.guid, Escape(h.name), h.account, h.earnedAt);
        LOG_WARN("module", "mod-temucraft-realm-first: {} got {} without the module (e.g. .achievement add): recorded as its holder",
            h.name, achievement->ID);
    }

private:
    static void Skill(Player* player, uint32 skillId, uint32 value, uint32 newValue)
    {
        if (!settings.enable || newValue <= value)
            return;
        std::vector<Entry> entries = FreeEntries([&](Entry const& e)
        {
            return e.type == TYPE_PROFESSION && e.skill == skillId && e.skillValue > value && e.skillValue <= newValue;
        });
        TryGrant(player, entries);
    }
};

class TemucraftRealmFirst_Achievement : public AchievementScript
{
public:
    TemucraftRealmFirst_Achievement() : AchievementScript("TemucraftRealmFirst_Achievement", { ACHIEVEMENTHOOK_IS_REALM_COMPLETED,
        ACHIEVEMENTHOOK_CAN_CHECK_CRITERIA, ACHIEVEMENTHOOK_IS_COMPLETED_CRITERIA }) { }

    // For the achievements the module decides, the holder table says whether it's taken (a reset reopens it).
    bool IsRealmCompleted(AchievementGlobalMgr const* /*globalmgr*/, AchievementEntry const* achievement, SystemTimePoint /*completionTime*/) override
    {
        if (!achievement || achievement->ID >= MAX_ACHIEVEMENT_ID || !sManaged[achievement->ID].load())
            return true;
        return Held(achievement->ID);
    }

    // Taken-over stock Realm Firsts: stock criteria never progress nor complete them.
    bool CanCheckCriteria(AchievementMgr* /*mgr*/, AchievementCriteriaEntry const* criteria) override
    {
        return !criteria || criteria->referredAchievement >= MAX_ACHIEVEMENT_ID || !sBlocked[criteria->referredAchievement].load(std::memory_order_relaxed);
    }

    bool IsCompletedCriteria(AchievementMgr* /*mgr*/, AchievementCriteriaEntry const* /*criteria*/, AchievementEntry const* achievement,
        CriteriaProgress const* /*progress*/) override
    {
        return !achievement || achievement->ID >= MAX_ACHIEVEMENT_ID || !sBlocked[achievement->ID].load(std::memory_order_relaxed);
    }
};

class TemucraftRealmFirst_Commands : public CommandScript
{
public:
    TemucraftRealmFirst_Commands() : CommandScript("TemucraftRealmFirst_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "list",     HandleList,     103901 /* SEC_GAMEMASTER -> RBAC */,    Console::Yes },
            { "eligible", HandleEligible, 103902 /* SEC_GAMEMASTER -> RBAC */,    Console::Yes },
            { "reset",    HandleReset,    103903 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
            { "mark",     HandleMark,     103904 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
            { "reload",   HandleReload,   103905 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
        };
        static ChatCommandTable root = { { "temucraft realmfirst", sub } };
        return root;
    }

    static void ShowTiers(ChatHandler* handler)
    {
        std::vector<uint8> tiers;
        {
            std::lock_guard<std::mutex> guard(sLock);
            for (Entry const& e : sCatalog)
                if (std::find(tiers.begin(), tiers.end(), e.tier) == tiers.end())
                    tiers.push_back(e.tier);
        }
        std::sort(tiers.begin(), tiers.end());
        handler->PSendSysMessage(T(handler, 3, "Realm firsts: module {}, expansion {}, stock firsts {}, random bots {}, alt bots {}."),
            settings.enable ? "on" : "off", Expansion(), settings.takeOverStock ? "taken over" : "left to the core",
            settings.excludeRandomBots ? "excluded" : "allowed", settings.excludeAltBots ? "excluded" : "allowed");
        for (uint8 tier : tiers)
        {
            uint32 openTime = 0;
            bool open = TierOpen(tier, &openTime);
            handler->PSendSysMessage(T(handler, 4, "  Tier {} ({}): {}"), tier, TierName(tier),
                open ? Acore::StringFormat("open since {}", Date(openTime))
                     : openTime ? Acore::StringFormat("opens on {}", Date(openTime)) : std::string("closed"));
        }
    }

    static bool HandleList(ChatHandler* handler, Optional<std::string> filter)
    {
        std::string what = filter ? *filter : "held";
        std::transform(what.begin(), what.end(), what.begin(), ::tolower);
        bool showHeld = what == "held" || what == "all";
        bool showOpen = what == "open" || what == "all";
        if (!showHeld && !showOpen)
        {
            handler->SendSysMessage(T(handler, 5, "Use: .temucraft realmfirst list [held|open|all]"));
            return true;
        }
        ShowTiers(handler);

        std::vector<Entry> catalog;
        std::unordered_map<uint32, Holder> holders;
        {
            std::lock_guard<std::mutex> guard(sLock);
            catalog = sCatalog;
            holders = sHolders;
        }
        std::sort(catalog.begin(), catalog.end(), [](Entry const& a, Entry const& b)
            { return a.tier != b.tier ? a.tier < b.tier : a.achievement < b.achievement; });
        uint32 held = 0, open = 0, off = 0;
        for (Entry const& e : catalog)
        {
            if (!sManaged[e.achievement].load())
            {
                ++off;
                continue;
            }
            auto itr = holders.find(e.achievement);
            if (itr != holders.end() && itr->second.held)
            {
                ++held;
                if (showHeld)
                    handler->PSendSysMessage(T(handler, 6, "  [{}] {}: {} ({}, {})"), e.achievement, AchievementName(e.achievement),
                        itr->second.name, Date(itr->second.earnedAt), itr->second.source);
            }
            else
            {
                ++open;
                if (showOpen)
                    handler->PSendSysMessage(T(handler, 7, "  [{}] {}: free{}"), e.achievement, AchievementName(e.achievement),
                        TierOpen(e.tier) ? "" : " (tier not open)");
            }
        }
        handler->PSendSysMessage(T(handler, 8, "{} held, {} free, {} not handled by the module (off, or stock left to the core)."),
            held, open, off);
        return true;
    }

    static bool HandleEligible(ChatHandler* handler, std::string name)
    {
        Optional<Target> target = FindTarget(name);
        if (!target)
        {
            handler->PSendSysMessage(T(handler, 9, "No character named {}."), name);
            return true;
        }
        Optional<Eligibility> row = LoadEligibility(target->guid.GetCounter());
        if (!row && !settings.excludeRandomBots && IsRandomBotAccount(target->account))
            row = LoadFromCharacters(target->guid.GetCounter());
        if (row)
            handler->PSendSysMessage(T(handler, 10, "{} (guid {}, account {}): row made {} on account {} as race {} class {} ({}), "
                "tiers open then: {:#x}, {}."), target->name, target->guid.GetCounter(), target->account, Date(row->createdAt),
                row->account, row->race, row->cls, row->source, row->tiersAtCreation,
                row->eligible ? std::string("not marked") : Acore::StringFormat("marked ineligible by {}: {}", row->markedBy, row->reason));
        else
            handler->PSendSysMessage(T(handler, 11, "{} (guid {}, account {}): no eligibility row, so no realm firsts (not created "
                "here while the module ran: pdump, copy, older character or bot). .temucraft realmfirst mark {} eligible adds one."),
                target->name, target->guid.GetCounter(), target->account, target->name);

        Boost boost = BoostOf(target->guid.GetCounter(), row);
        if (boost.Any())
            handler->PSendSysMessage(T(handler, 23, "  Boosted by GM commands, so never eligible for: {}{}. Why: {}"),
                boost.levels ? "level firsts" : "", boost.allSkills ? (boost.levels ? ", every profession" : "every profession")
                : boost.skills.empty() ? "" : Acore::StringFormat("{}skills {}", boost.levels ? ", " : "", SkillsText(boost)), boost.reason);

        if (target->online)
        {
            std::string bot = BotVerdict(target->online);
            if (!bot.empty())
                handler->PSendSysMessage(T(handler, 12, "  Online now as: {} (excluded)."), bot);
            if (target->online->IsGameMaster())
                handler->SendSysMessage(T(handler, 13, "  GM mode is on: nothing is earned while it is."));
        }
        else if (IsRandomBotAccount(target->account) && settings.excludeRandomBots)
            handler->SendSysMessage(T(handler, 14, "  Random bot account: excluded."));

        std::vector<uint8> tiers;
        {
            std::lock_guard<std::mutex> guard(sLock);
            for (Entry const& e : sCatalog)
                if (std::find(tiers.begin(), tiers.end(), e.tier) == tiers.end())
                    tiers.push_back(e.tier);
        }
        std::sort(tiers.begin(), tiers.end());
        for (uint8 tier : tiers)
        {
            std::string why = CharacterVerdict(row, target->account, target->race, target->cls, tier, 0);
            uint32 free = FreeEntries([&](Entry const& e) { return e.tier == tier; }).size();
            handler->PSendSysMessage(T(handler, 15, "  Tier {} ({}): {} ({} still free)."), tier, TierName(tier),
                why.empty() ? std::string("eligible") : "not eligible: " + why, free);
        }
        return true;
    }

    static bool HandleReset(ChatHandler* handler, uint32 achievement, Optional<std::string> keep)
    {
        bool keepIt = keep && (*keep == "keep" || *keep == "KEEP");
        bool known;
        {
            std::lock_guard<std::mutex> guard(sLock);
            known = sById.count(achievement) != 0;
        }
        if (!known || achievement >= MAX_ACHIEVEMENT_ID || !sManaged[achievement].load())
        {
            handler->PSendSysMessage(T(handler, 16, "{} is not a realm first the module handles (see .temucraft realmfirst list all)."), achievement);
            return true;
        }
        Holder holder;
        if (!Held(achievement, &holder))
        {
            handler->PSendSysMessage(T(handler, 17, "{} is already free."), achievement);
            return true;
        }
        if (!keepIt && holder.guid)
        {
            if (ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(holder.guid)))
            {
                handler->PSendSysMessage(T(handler, 18, "{} is online: the achievement can only be taken from an offline character. "
                    "Ask them to log out, or add \"keep\" to reopen it and let them keep it."), holder.name);
                return true;
            }
            CharacterDatabase.Execute("DELETE FROM `character_achievement` WHERE `guid` = {} AND `achievement` = {}", holder.guid, achievement);
        }
        {
            std::lock_guard<std::mutex> guard(sLock);
            sHolders[achievement].held = false;
            sHolders[achievement].source = "reset";
        }
        CharacterDatabase.Execute("UPDATE `mod_temucraft_realm_first_holder` SET `held` = 0, `source` = 'reset', `note` = '{}' "
            "WHERE `achievement` = {}", Escape(Acore::StringFormat("was {} ({}), reset by {} on {}{}", holder.name, holder.guid,
            ByName(handler), Date(Now()), keepIt ? ", kept" : "")), achievement);
        LOG_INFO("module", "mod-temucraft-realm-first: {} reset {} (held by {}{})", ByName(handler), achievement, holder.name,
            keepIt ? ", who keeps it" : "");
        handler->PSendSysMessage(T(handler, 19, "{} is free again ({} {})."), AchievementName(achievement), holder.name,
            keepIt ? "keeps it" : "lost it");
        return true;
    }

    static bool HandleMark(ChatHandler* handler, std::string name, std::string state, Optional<Tail> reason)
    {
        std::transform(state.begin(), state.end(), state.begin(), ::tolower);
        if (state != "eligible" && state != "ineligible")
        {
            handler->SendSysMessage(T(handler, 20, "Use: .temucraft realmfirst mark <name> eligible|ineligible [reason]"));
            return true;
        }
        Optional<Target> target = FindTarget(name);
        if (!target)
        {
            handler->PSendSysMessage(T(handler, 9, "No character named {}."), name);
            return true;
        }
        bool eligible = state == "eligible";
        std::string why = reason ? std::string(*reason) : std::string();
        uint32 guid = target->guid.GetCounter();
        if (LoadEligibility(guid))
            CharacterDatabase.Execute("UPDATE `mod_temucraft_realm_first_eligibility` SET `eligible` = {}, `reason` = '{}', `marked_by` = '{}', "
                "`marked_at` = {} WHERE `guid` = {}", eligible ? 1 : 0, Escape(why), Escape(ByName(handler)), Now(), guid);
        else
            // no row yet: made with the character's creation date from the characters table, so the tier rule still applies
            CharacterDatabase.Execute("INSERT INTO `mod_temucraft_realm_first_eligibility` (`guid`, `account`, `name`, `race`, `class`, "
                "`created_at`, `tiers_at_creation`, `eligible`, `source`, `reason`, `marked_by`, `marked_at`) "
                "SELECT `guid`, `account`, `name`, `race`, `class`, UNIX_TIMESTAMP(`creation_date`), 0, {}, 'marked', '{}', '{}', {} "
                "FROM `characters` WHERE `guid` = {}", eligible ? 1 : 0, Escape(why), Escape(ByName(handler)), Now(), guid);
        LOG_INFO("module", "mod-temucraft-realm-first: {} marked {} {}{}{}", ByName(handler), target->name, state,
            why.empty() ? "" : ": ", why);
        handler->PSendSysMessage(T(handler, 21, "{} is now marked {}. .temucraft realmfirst eligible {} shows the result."),
            target->name, state, target->name);
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadAll();
        handler->SendSysMessage(T(handler, 22, "Realm firsts: catalog, tiers and holders re-read."));
        return true;
    }
};

// GM commands that level a character, set its skills or teach it profession spells: the character may never earn the
// firsts they boosted it towards. Runs before the command (it marks even if the command then fails on its arguments),
// but only when the invoker has the command's permission.
class TemucraftRealmFirst_CommandWatch : public AllCommandScript
{
public:
    TemucraftRealmFirst_CommandWatch() : AllCommandScript("TemucraftRealmFirst_CommandWatch", { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        if (settings.enable && settings.markGmBoosts)
            Watch(handler, cmdStr);
        return true;
    }

private:
    // Hyperlinks become one token "@type:id:...", colours and the link text go.
    static std::vector<std::string> Tokens(std::string_view text)
    {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '|' && i + 1 < text.size())
            {
                char c = text[i + 1];
                if (c == 'c') { i += 9; continue; }
                if (c == 'r') { ++i; continue; }
                if (c == 'H')
                {
                    size_t end = text.find("|h", i + 2);
                    if (end == std::string_view::npos)
                        break;
                    out += " @" + std::string(text.substr(i + 2, end - i - 2)) + " ";
                    size_t close = text.find("]|h", end + 2);
                    i = close == std::string_view::npos ? end + 1 : close + 2;
                    continue;
                }
            }
            out += text[i];
        }
        std::vector<std::string> tokens;
        for (std::string_view t : Acore::Tokenize(out, ' ', false))
            tokens.emplace_back(t);
        return tokens;
    }

    static Optional<uint32> Id(std::string const& token, char const* type)
    {
        std::string prefix = Acore::StringFormat("@{}:", type);
        if (StringStartsWithI(token, prefix))
        {
            std::string rest = token.substr(prefix.size());
            return Acore::StringTo<uint32>(rest.substr(0, rest.find(':')));
        }
        return Acore::StringTo<uint32>(token);
    }

    static void Watch(ChatHandler& handler, std::string_view cmdStr)
    {
        std::vector<std::string> tok = Tokens(cmdStr);
        auto is = [&](size_t i, std::string_view name)
        {
            return i < tok.size() && !tok[i].empty() && tok[i][0] != '@' && StringStartsWithI(name, tok[i]);
        };

        enum TargetMode { NAME_OR_SELECTED_OR_SELF, SELECTED, SELF };
        uint32 permission = 0;
        bool levels = false, allSkills = false;
        std::set<uint32> skills;
        TargetMode mode = NAME_OR_SELECTED_OR_SELF;
        size_t args = 1;

        if (is(0, "levelup"))
            permission = rbac::RBAC_PERM_COMMAND_LEVELUP, levels = true;
        else if (is(0, "character") && is(1, "level"))
            permission = rbac::RBAC_PERM_COMMAND_CHARACTER_LEVEL, levels = true, args = 2;
        else if (is(0, "quest") && is(1, "complete"))
            permission = rbac::RBAC_PERM_COMMAND_QUEST_COMPLETE, levels = true, args = 2;
        else if (is(0, "quest") && is(1, "reward"))
            permission = rbac::RBAC_PERM_COMMAND_QUEST_REWARD, levels = true, args = 2;
        else if (is(0, "setskill") && tok.size() > 1)
        {
            Optional<uint32> skill = Id(tok[1], "skill");
            if (!skill)
                return;
            permission = rbac::RBAC_PERM_COMMAND_SETSKILL, mode = SELECTED;
            skills.insert(*skill);
        }
        else if (is(0, "learn") && is(1, "all") && is(2, "crafts"))
            permission = rbac::RBAC_PERM_COMMAND_LEARN_ALL_CRAFTS, allSkills = true, mode = SELF;
        else if (is(0, "learn") && is(1, "all") && is(2, "recipes"))
            permission = rbac::RBAC_PERM_COMMAND_LEARN_ALL_RECIPES, allSkills = true, mode = SELECTED;
        else if (is(0, "learn") && tok.size() > 1 && !is(1, "all"))
        {
            Optional<uint32> spell = Id(tok[1], "spell");
            SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(*spell) : nullptr;
            if (!info)
                return;
            std::set<uint32> professions;
            {
                std::lock_guard<std::mutex> guard(sLock);
                for (Entry const& e : sCatalog)
                    if (e.type == TYPE_PROFESSION)
                        professions.insert(e.skill);
            }
            // a profession rank (Journeyman Tailoring...) or a recipe of a profession
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if ((info->Effects[i].Effect == SPELL_EFFECT_SKILL || info->Effects[i].Effect == SPELL_EFFECT_SKILL_STEP) &&
                    professions.count(uint32(info->Effects[i].MiscValue)))
                    skills.insert(uint32(info->Effects[i].MiscValue));
            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(info->Id);
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
                if (professions.count(itr->second->SkillLine))
                    skills.insert(itr->second->SkillLine);
            if (skills.empty())
                return;
            permission = rbac::RBAC_PERM_COMMAND_LEARN, mode = SELECTED;
        }
        else
            return;

        if (!handler.IsConsole() && !handler.GetSession()->HasPermission(permission))
            return;     // the command will be refused

        ObjectGuid target;
        if (mode == NAME_OR_SELECTED_OR_SELF)
            for (size_t i = args; i < tok.size() && !target; ++i)
            {
                std::string name = tok[i];
                if (StringStartsWithI(name, "@player:"))    // a player link
                    name = name.substr(8, name.find(':', 8) == std::string::npos ? std::string::npos : name.find(':', 8) - 8);
                if (name.empty() || name[0] == '@' || name[0] == '-' || std::isdigit(uint8(name[0])) || !normalizePlayerName(name))
                    continue;
                if (CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByName(name))
                    target = cache->Guid;
            }
        if (!target && !handler.IsConsole())
        {
            Player* player = mode == SELF ? handler.GetSession()->GetPlayer()
                           : mode == SELECTED ? handler.getSelectedPlayer() : handler.getSelectedPlayerOrSelf();
            if (player)
                target = player->GetGUID();
        }
        if (!target)
            return;

        std::string why = Acore::StringFormat(".{} by {} on {}", std::string(cmdStr).substr(0, 80), ByName(&handler), Date(Now()));
        Mark(target.GetCounter(), levels, allSkills, skills, why);
    }

    static void Mark(ObjectGuid::LowType guid, bool levels, bool allSkills, std::set<uint32> const& skills, std::string const& why)
    {
        Optional<Eligibility> row = LoadEligibility(guid);
        Boost b = BoostOf(guid, row);
        b.levels |= levels;
        b.allSkills |= allSkills;
        b.skills.insert(skills.begin(), skills.end());
        b.reason = (why + (b.reason.empty() ? "" : "; " + b.reason)).substr(0, 255);
        {
            std::lock_guard<std::mutex> guard(sLock);
            sBoosts[guid] = b;
        }
        if (row)
            CharacterDatabase.Execute("UPDATE `mod_temucraft_realm_first_eligibility` SET `boost_levels` = {}, `boost_skills` = '{}', "
                "`boost_reason` = '{}' WHERE `guid` = {}", b.levels ? 1 : 0, SkillsText(b), Escape(b.reason), guid);
        else
            CharacterDatabase.Execute("INSERT IGNORE INTO `mod_temucraft_realm_first_eligibility` (`guid`, `account`, `name`, `race`, "
                "`class`, `created_at`, `eligible`, `source`, `reason`, `boost_levels`, `boost_skills`, `boost_reason`) SELECT `guid`, "
                "`account`, `name`, `race`, `class`, UNIX_TIMESTAMP(`creation_date`), 0, 'boosted', 'no eligibility row, boosted by a GM "
                "command', {}, '{}', '{}' FROM `characters` WHERE `guid` = {}", b.levels ? 1 : 0, SkillsText(b), Escape(b.reason), guid);
        LOG_INFO("module", "mod-temucraft-realm-first: character {} boosted ({}{}): {}", guid, levels ? "levels " : "",
            allSkills ? "all professions" : SkillsText(b), why);
    }
};

class TemucraftRealmFirst_World : public WorldScript
{
public:
    TemucraftRealmFirst_World() : WorldScript("TemucraftRealmFirst_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        settings.enable = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.Enable", true);
        settings.expansion = sConfigMgr->GetOption<int32>("Temucraft.RealmFirst.Expansion", -1);
        settings.excludeRandomBots = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.ExcludeRandomBots", true);
        settings.excludeAltBots = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.ExcludeAltBots", false);
        settings.randomBotPrefix = sConfigMgr->GetOption<std::string>("Temucraft.RealmFirst.RandomBotAccountPrefix", "RNDBOT");
        settings.announce = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.Announce", true);
        settings.tellIneligible = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.TellIneligible", true);
        settings.takeOverStock = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.TakeOverStock", true);
        settings.sameAccount = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.RequireSameAccount", true);
        settings.existingEligible = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.ExistingCharactersEligible", true);
        settings.markGmBoosts = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.MarkGmBoosted", true);
        settings.types[TYPE_LEVEL] = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.Overall", true);
        settings.types[TYPE_CLASS] = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.Class", true);
        settings.types[TYPE_RACE] = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.Race", true);
        settings.types[TYPE_PROFESSION] = sConfigMgr->GetOption<bool>("Temucraft.RealmFirst.Profession", true);
        for (uint32 tier = 0; tier < MAX_TIERS; ++tier)
            settings.openDate[tier] = ParseDate(sConfigMgr->GetOption<std::string>(
                Acore::StringFormat("Temucraft.RealmFirst.Tier{}.OpenDate", tier), "", tier < 3));
        settings.zoneAccessExpansion = sConfigMgr->GetOption<int32>("Temucraft.ZoneAccess.Expansion", 2, false);
        {
            std::lock_guard<std::mutex> guard(sLock);
            sRandomBotAccount.clear();
        }
        if (reload)
            LoadAll();
    }

    void OnStartup() override
    {
        LoadAll();
    }
};

void AddTemucraftRealmFirstScripts()
{
    new TemucraftRealmFirst_Player();
    new TemucraftRealmFirst_Achievement();
    new TemucraftRealmFirst_Commands();
    new TemucraftRealmFirst_CommandWatch();
    new TemucraftRealmFirst_World();
}
