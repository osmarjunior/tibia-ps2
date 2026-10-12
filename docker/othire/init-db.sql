-- OTHire Docker init: runs automatically when MariaDB container first starts.
-- Creates database + loads schema.
-- NOTE: The MySql_Schema.sql from OTHire is very old (2014).
--       This is a clean, compatible version with the same tables.

CREATE DATABASE IF NOT EXISTS othire CHARACTER SET latin1;
USE othire;

CREATE TABLE IF NOT EXISTS `accounts` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(32) NOT NULL DEFAULT '',
  `password` varchar(255) NOT NULL DEFAULT '',
  `premend` int(11) NOT NULL DEFAULT '0',
  `type` int(11) NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`),
  UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

CREATE TABLE IF NOT EXISTS `players` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(32) NOT NULL,
  `account_id` int(11) NOT NULL DEFAULT '0',
  `group_id` int(11) NOT NULL DEFAULT '1',
  `sex` int(11) NOT NULL DEFAULT '0',
  `vocation` int(11) NOT NULL DEFAULT '0',
  `experience` bigint(20) NOT NULL DEFAULT '0',
  `level` int(11) NOT NULL DEFAULT '1',
  `health` int(11) NOT NULL DEFAULT '150',
  `healthmax` int(11) NOT NULL DEFAULT '150',
  `mana` int(11) NOT NULL DEFAULT '0',
  `manamax` int(11) NOT NULL DEFAULT '0',
  `lookbody` int(11) NOT NULL DEFAULT '0',
  `lookfeet` int(11) NOT NULL DEFAULT '0',
  `lookhead` int(11) NOT NULL DEFAULT '0',
  `looklegs` int(11) NOT NULL DEFAULT '0',
  `looktype` int(11) NOT NULL DEFAULT '136',
  `posx` int(11) NOT NULL DEFAULT '0',
  `posy` int(11) NOT NULL DEFAULT '0',
  `posz` int(11) NOT NULL DEFAULT '0',
  `cap` int(11) NOT NULL DEFAULT '400',
  `lastlogin` bigint(20) NOT NULL DEFAULT '0',
  `lastip` int(10) unsigned NOT NULL DEFAULT '0',
  `save` int(11) NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`),
  UNIQUE KEY `name` (`name`),
  KEY `account_id` (`account_id`),
  CONSTRAINT `players_ibfk_1` FOREIGN KEY (`account_id`) REFERENCES `accounts` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- Test account + player
INSERT IGNORE INTO `accounts` (`id`, `name`, `password`, `type`) VALUES
  (1, '111111', 'tibia', 1);

INSERT IGNORE INTO `players` (`id`, `name`, `account_id`, `group_id`, `sex`,
  `vocation`, `experience`, `level`, `health`, `healthmax`, `mana`, `manamax`,
  `lookbody`, `lookfeet`, `lookhead`, `looklegs`, `looktype`,
  `posx`, `posy`, `posz`, `cap`, `save`) VALUES
  (1, 'Ps2 Tester', 1, 1, 1,
   1, 4200, 8, 185, 185, 35, 35,
   20, 40, 10, 30, 129,
  996, 1000, 7, 470, 1);
