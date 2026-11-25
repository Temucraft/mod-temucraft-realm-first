-- mod-temucraft-realm-first: who may earn a Realm First!, who holds each one, and when each content tier opened.
-- Never emptied: re-running this file keeps every row.

-- One row per character, written when the character is created on this realm (OnPlayerCreate). A character with no
-- row (loaded with pdump, copied by hand, created before the module, made by the random bot factory) is not eligible.
--   created_at          unix time of creation: a tier's firsts need created_at >= the tier's open time
--   tiers_at_creation   bit N = tier N was open when the character was made (for the audit; the rule uses created_at)
--   eligible            0 = marked ineligible by a GM (.temucraft realmfirst mark), whatever the rest says
--   source              'created', 'marked' (row made by .temucraft realmfirst mark)
CREATE TABLE IF NOT EXISTS `mod_temucraft_realm_first_eligibility` (
  `guid` int unsigned NOT NULL,
  `account` int unsigned NOT NULL,
  `name` varchar(12) NOT NULL DEFAULT '',
  `race` tinyint unsigned NOT NULL DEFAULT 0,
  `class` tinyint unsigned NOT NULL DEFAULT 0,
  `created_at` int unsigned NOT NULL DEFAULT 0,
  `tiers_at_creation` int unsigned NOT NULL DEFAULT 0,
  `eligible` tinyint unsigned NOT NULL DEFAULT 1,
  `source` varchar(16) NOT NULL DEFAULT 'created',
  `reason` varchar(255) NOT NULL DEFAULT '',
  `marked_by` varchar(32) NOT NULL DEFAULT '',
  `marked_at` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`),
  KEY `account` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-realm-first';

-- One row per Realm First! the module decides. held = 0: reopened by .temucraft realmfirst reset (the next eligible
-- first earns it again). source: 'module' (granted by the module), 'imported' (found in character_achievement when the
-- module first ran), 'other' (given some other way, e.g. .achievement add), 'reset' (reopened).
CREATE TABLE IF NOT EXISTS `mod_temucraft_realm_first_holder` (
  `achievement` int unsigned NOT NULL,
  `guid` int unsigned NOT NULL DEFAULT 0,
  `name` varchar(12) NOT NULL DEFAULT '',
  `account` int unsigned NOT NULL DEFAULT 0,
  `earned_at` int unsigned NOT NULL DEFAULT 0,
  `held` tinyint unsigned NOT NULL DEFAULT 1,
  `source` varchar(16) NOT NULL DEFAULT 'module',
  `note` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`achievement`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-realm-first';

-- When each tier opened, as the module first saw it open (used when Temucraft.RealmFirst.Tier<N>.OpenDate is empty).
-- Delete a row to stamp that tier again at the next start (or set its OpenDate in the config instead).
CREATE TABLE IF NOT EXISTS `mod_temucraft_realm_first_tier` (
  `tier` tinyint unsigned NOT NULL,
  `opened_at` int unsigned NOT NULL,
  PRIMARY KEY (`tier`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-realm-first';
