#!/usr/bin/env python3
"""Builds mod-temucraft-realm-first's achievement rows from the lists below.

Writes (all re-runnable, nothing else is touched):
  data/sql/db-world/base/mod_temucraft_realm_first.sql      the catalog the module reads (new rows and the stock
                                                            Realm First! rows it can take over)
  data/sql/db-world/base/mod_temucraft_realm_first_dbc.sql  achievement_dbc rows for the new achievements (the server
                                                            knows them even before its DBCs are rebuilt)
  client/achievements.tsv                                   the new rows, readable
  client/patch-T.MPQ/DBFilesClient/Achievement.dbc          only with --base-dbc: that Achievement.dbc plus the new rows

Usage:
  python build_realm_first.py                               SQL and TSV only
  python build_realm_first.py --base-dbc Achievement.dbc    also the client DBC (give the client's current file:
                                                            today DBFilesClient\\Achievement.dbc of patch-5.MPQ)

The new achievements have no criteria (nothing in Achievement_Criteria.dbc): no stock code can ever complete them,
only the module grants them. Category 81 (Feats of Strength), 0 points, flag 0x100 (ACHIEVEMENT_FLAG_REALM_FIRST_REACH:
the client's "Realm First!" broadcast and the core's realm-wide bookkeeping).
"""
import argparse, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)

FIRST_ID, LAST_ID = 60000, 60299          # reserved range
CATEGORY, POINTS, FLAGS = 81, 0, 0x100    # Feats of Strength, 0 points, REALM_FIRST_REACH
UI_ORDER = 300
LANG_MASK, REWARD_MASK = 0xff01fe, 0xff01ee   # as on the stock Realm First! rows

TIERS = {0: (60, 300, 'Artisan'), 1: (70, 375, 'Master'), 2: (80, 450, 'Grand Master')}

# class id: (name, icon, stock level 80 achievement)
CLASSES = {1: ('Warrior', 1462, 459), 2: ('Paladin', 25, 465), 3: ('Hunter', 2831, 462), 4: ('Rogue', 1834, 458),
           5: ('Priest', 1523, 464), 6: ('Death Knight', 2639, 461), 7: ('Shaman', 38, 467), 8: ('Mage', 2832, 460),
           9: ('Warlock', 155, 463), 11: ('Druid', 261, 466)}

# race id (ChrRaces.dbc of the client): (title name, description name, stock or WHE level 80 achievement, its icon)
RACES = {1: ('Human', 'human', 1408, 3325), 2: ('Orc', 'orc', 1410, 3289), 3: ('Dwarf', 'dwarf', 1407, 3286),
         4: ('Night Elf', 'night elf', 1409, 3288), 5: ('Forsaken', 'forsaken', 1413, 3292),
         6: ('Tauren', 'tauren', 1411, 3326), 7: ('Gnome', 'gnome', 1404, 3283), 8: ('Troll', 'troll', 1412, 3291),
         9: ('Goblin', 'goblin', 1429, 463874), 10: ('Blood Elf', 'blood elf', 1405, 3323),
         11: ('Draenei', 'draenei', 1406, 3324), 12: ('Worgen', 'worgen', 1430, 463876),
         13: ('Pandaren (Alliance)', 'Alliance pandaren', 1437, 463876),
         14: ('Pandaren (Horde)', 'Horde pandaren', 1438, 463874),
         15: ('Nightborne', 'nightborne', 1454, 3323), 16: ('Highmountain Tauren', 'Highmountain tauren', 1442, 3323),
         17: ('Void Elf', 'void elf', 1445, 463876), 18: ('Lightforged Draenei', 'Lightforged draenei', 1446, 463876),
         19: ('Zandalari Troll', 'Zandalari troll', 1435, 3323), 20: ('Kul Tiran', 'Kul Tiran', 1440, 463876),
         21: ('Vulpera', 'vulpera', 1436, 3323), 22: ('Dark Iron Dwarf', 'Dark Iron dwarf', 1434, 3323),
         23: ("Mag'har", "Mag'har orc", 1432, 3323), 24: ('Mechagnome', 'mechagnome', 1450, 463876),
         25: ('Dracthyr (Alliance)', 'Alliance dracthyr', 1529, 3323),
         26: ('Dracthyr (Horde)', 'Horde dracthyr', 1530, 3323),
         27: ('Earthen (Alliance)', 'Alliance earthen', 1448, 463876),
         28: ('Earthen (Horde)', 'Horde earthen', 1449, 463876),
         29: ('Haranir (Alliance)', 'Alliance haranir', 1534, 3323),
         30: ('Haranir (Horde)', 'Horde haranir', 1543, 3323)}

# skill id: (description name, title for 300, title for 375, stock 450 achievement, icon)
PROFESSIONS = [
    (164, 'blacksmithing', 'Artisan Blacksmith', 'Master Blacksmith', 1414, 335),
    (171, 'alchemy', 'Artisan Alchemist', 'Master Alchemist', 1415, 339),
    (185, 'cooking', 'Cooking Artisan', 'Cooking Master', 1416, 1467),
    (333, 'enchanting', 'Artisan Enchanter', 'Master Enchanter', 1417, 578),
    (202, 'engineering', 'Artisan Engineer', 'Master Engineer', 1418, 333),
    (129, 'first aid', 'First Aid Artisan', 'First Aid Master', 1419, 504),
    (356, 'fishing', 'Artisan Angler', 'Master Angler', 1420, 580),
    (182, 'herbalism', 'Artisan Herbalist', 'Master Herbalist', 1421, 345),
    (773, 'inscription', 'Artisan Scribe', 'Master Scribe', 1422, 2557),
    (755, 'jewelcrafting', 'Artisan Jewelcrafter', 'Master Jewelcrafter', 1423, 1768),
    (165, 'leatherworking', 'Artisan Leatherworker', 'Master Leatherworker', 1424, 346),
    (186, 'mining', 'Artisan Miner', 'Master Miner', 1425, 336),
    (393, 'skinning', 'Artisan Skinner', 'Master Skinner', 1426, 736),
    (197, 'tailoring', 'Artisan Tailor', 'Master Tailor', 1427, 341),
]

TYPE_LEVEL, TYPE_CLASS, TYPE_RACE, TYPE_PROFESSION = 0, 1, 2, 3


def catalog():
    """Rows: (id, type, tier, level, class, race, skill, skill value, stock, title, description, icon)."""
    rows = []
    for tier in (0, 1):
        level, skill_value, _ = TIERS[tier]
        base = FIRST_ID + tier * 100
        rows.append((base, TYPE_LEVEL, tier, level, 0, 0, 0, 0, 0, f'Realm First! Level {level}',
                     f'First person on the realm to achieve level {level}.', 3275))
        for cls, (name, icon, _) in CLASSES.items():
            rows.append((base + cls, TYPE_CLASS, tier, level, cls, 0, 0, 0, 0, f'Realm First! Level {level} {name}',
                         f'First {name.lower()} on the realm to achieve level {level}.', icon))
        for race, (title, desc, _, icon) in RACES.items():
            rows.append((base + 20 + race, TYPE_RACE, tier, level, 0, race, 0, 0, 0,
                         f'Realm First! Level {level} {title}',
                         f'First {desc} on the realm to achieve level {level}.', icon))
        for i, (skill, desc, t300, t375, _, icon) in enumerate(PROFESSIONS):
            rows.append((FIRST_ID + 200 + tier * 50 + i, TYPE_PROFESSION, tier, 0, 0, 0, skill, skill_value, 0,
                         f'Realm First! {t300 if tier == 0 else t375}',
                         f'First person on the realm to achieve {skill_value} skill in {desc}.', icon))
    # the stock (and WHE) level 80 / 450 rows: taken over only with Temucraft.RealmFirst.TakeOverStock
    rows.append((457, TYPE_LEVEL, 2, 80, 0, 0, 0, 0, 1, 'Realm First! Level 80', '(stock)', 0))
    for cls, (name, _, ach) in CLASSES.items():
        rows.append((ach, TYPE_CLASS, 2, 80, cls, 0, 0, 0, 1, f'Realm First! Level 80 {name}', '(stock)', 0))
    for race, (title, _, ach, _) in RACES.items():
        rows.append((ach, TYPE_RACE, 2, 80, 0, race, 0, 0, 1, f'Realm First! Level 80 {title}', '(stock)', 0))
    for skill, desc, _, _, ach, _ in PROFESSIONS:
        rows.append((ach, TYPE_PROFESSION, 2, 0, 0, 0, skill, 450, 1, f'Realm First! 450 {desc}', '(stock)', 0))
    new = [r for r in rows if not r[8]]
    assert all(FIRST_ID <= r[0] <= LAST_ID for r in new)
    assert len({r[0] for r in rows}) == len(rows)
    return rows


def q(s):
    return "'" + s.replace('\\', '\\\\').replace("'", "''") + "'"


def write_sql(rows):
    new = [r for r in rows if not r[8]]
    out = os.path.join(MOD, 'data', 'sql', 'db-world', 'base')
    with open(os.path.join(out, 'mod_temucraft_realm_first.sql'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('-- mod-temucraft-realm-first: the Realm First! achievements the module grants (made by tools/build_realm_first.py).\n'
                '-- Type 0 = overall level, 1 = class level, 2 = race level, 3 = profession skill. Tier 0 = Vanilla, 1 = TBC,\n'
                '-- 2 = Wrath (any number works, see the README). Stock = 1: a stock achievement, handled by the module only\n'
                '-- with Temucraft.RealmFirst.TakeOverStock = 1. Enabled = 0 turns a row off. Re-runnable: deletes its own rows.\n'
                'CREATE TABLE IF NOT EXISTS `mod_temucraft_realm_first` (\n'
                '  `Achievement` int unsigned NOT NULL,\n'
                '  `Type` tinyint unsigned NOT NULL,\n'
                '  `Tier` tinyint unsigned NOT NULL,\n'
                '  `Level` tinyint unsigned NOT NULL DEFAULT 0,\n'
                '  `Class` tinyint unsigned NOT NULL DEFAULT 0,\n'
                '  `Race` tinyint unsigned NOT NULL DEFAULT 0,\n'
                '  `Skill` smallint unsigned NOT NULL DEFAULT 0,\n'
                '  `SkillValue` smallint unsigned NOT NULL DEFAULT 0,\n'
                '  `Stock` tinyint unsigned NOT NULL DEFAULT 0,\n'
                '  `Enabled` tinyint unsigned NOT NULL DEFAULT 1,\n'
                '  `Comment` varchar(100) NOT NULL DEFAULT \'\',\n'
                '  PRIMARY KEY (`Achievement`)\n'
                ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT=\'mod-temucraft-realm-first\';\n\n')
        ids = ', '.join(str(r[0]) for r in rows if r[8])
        f.write(f'DELETE FROM `mod_temucraft_realm_first` WHERE `Achievement` BETWEEN {FIRST_ID} AND {LAST_ID} OR `Achievement` IN ({ids});\n')
        f.write('INSERT INTO `mod_temucraft_realm_first` (`Achievement`, `Type`, `Tier`, `Level`, `Class`, `Race`, `Skill`, `SkillValue`, `Stock`, `Enabled`, `Comment`) VALUES\n')
        f.write(',\n'.join(f'({r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {r[5]}, {r[6]}, {r[7]}, {r[8]}, 1, {q(r[9])})' for r in rows))
        f.write(';\n')
    cols = (['ID', 'Faction', 'Instance_Id', 'Supercedes'] +
            [f'Title_Lang_{l}' for l in LOCALES] + ['Title_Lang_Mask'] +
            [f'Description_Lang_{l}' for l in LOCALES] + ['Description_Lang_Mask'] +
            ['Category', 'Points', 'Ui_Order', 'Flags', 'IconID'] +
            [f'Reward_Lang_{l}' for l in LOCALES] + ['Reward_Lang_Mask', 'Minimum_Criteria', 'Shares_Criteria'])
    with open(os.path.join(out, 'mod_temucraft_realm_first_dbc.sql'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'-- mod-temucraft-realm-first: achievements {FIRST_ID}-{LAST_ID} for the server (the client needs the same rows in its\n'
                '-- Achievement.dbc: see client/). No criteria rows: only the module grants them. Re-runnable.\n')
        f.write(f'DELETE FROM `achievement_dbc` WHERE `ID` BETWEEN {FIRST_ID} AND {LAST_ID};\n')
        f.write('INSERT INTO `achievement_dbc` (' + ', '.join(f'`{c}`' for c in cols) + ') VALUES\n')
        lines = []
        for i, r in enumerate(new):
            v = ['%d' % r[0], '-1', '-1', '0'] + [q(r[9])] + ['NULL'] * 15 + ['%d' % LANG_MASK] + \
                [q(r[10])] + ['NULL'] * 15 + ['%d' % LANG_MASK] + \
                ['%d' % CATEGORY, '%d' % POINTS, '%d' % (UI_ORDER + i), '%d' % FLAGS, '%d' % r[11]] + \
                ['NULL'] * 16 + ['%d' % REWARD_MASK, '0', '0']
            assert len(v) == len(cols)
            lines.append('(' + ', '.join(v) + ')')
        f.write(',\n'.join(lines) + ';\n')
    with open(os.path.join(MOD, 'client', 'achievements.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('ID\tType\tTier\tLevel\tClass\tRace\tSkill\tSkillValue\tIcon\tTitle\tDescription\n')
        for r in new:
            f.write('\t'.join(str(x) for x in (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[11], r[9], r[10])) + '\n')
    print(f'{len(rows)} catalog rows ({len(new)} new achievements)')


LOCALES = ['enUS', 'enGB', 'koKR', 'frFR', 'deDE', 'enCN', 'zhCN', 'enTW', 'zhTW', 'esES', 'esMX', 'ruRU', 'ptPT',
           'ptBR', 'itIT', 'Unk']


def write_dbc(rows, base):
    """Achievement.dbc (3.3.5a: 62 fields) = base + the new rows. Rows already there with our IDs are replaced."""
    data = open(base, 'rb').read()
    magic, n, fields, size, ssize = struct.unpack_from('<4s4I', data)
    if magic != b'WDBC' or fields != 62 or size != 248:
        raise SystemExit(f'{base}: not a 3.3.5a Achievement.dbc')
    recs = [list(struct.unpack_from('<62I', data, 20 + i * size)) for i in range(n)]
    strings = bytearray(data[20 + n * size:])
    recs = [r for r in recs if not FIRST_ID <= r[0] <= LAST_ID]
    taken = {r[0] for r in recs}

    def add(s):
        off = len(strings)
        strings.extend(s.encode('utf-8') + b'\0')
        return off

    new = [r for r in rows if not r[8]]
    for i, r in enumerate(new):
        assert r[0] not in taken
        rec = [0] * 62
        rec[0], rec[1], rec[2], rec[3] = r[0], 0xFFFFFFFF, 0xFFFFFFFF, 0
        rec[4] = add(r[9]); rec[20] = LANG_MASK
        rec[21] = add(r[10]); rec[37] = LANG_MASK
        rec[38], rec[39], rec[40], rec[41], rec[42] = CATEGORY, POINTS, UI_ORDER + i, FLAGS, r[11]
        rec[59] = REWARD_MASK
        recs.append(rec)
    dest = os.path.join(MOD, 'client', 'patch-T.MPQ', 'DBFilesClient', 'Achievement.dbc')
    with open(dest, 'wb') as f:
        f.write(struct.pack('<4s4I', b'WDBC', len(recs), 62, 248, len(strings)))
        for rec in recs:
            f.write(struct.pack('<62I', *rec))
        f.write(strings)
    print(f'{dest}: {len(recs)} records ({n} in the base, {len(new)} new)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base-dbc', help="the client's current Achievement.dbc")
    a = ap.parse_args()
    rows = catalog()
    write_sql(rows)
    if a.base_dbc:
        write_dbc(rows, a.base_dbc)


if __name__ == '__main__':
    main()
