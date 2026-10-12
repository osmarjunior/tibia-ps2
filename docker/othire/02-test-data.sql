-- OTHire test data: minimal insert for test account/player
-- Only non-DEFAULT columns are specified; others use defaults

INSERT IGNORE INTO `accounts` (`id`, `password`, `email`, `premend`, `blocked`, `warnings`) VALUES
  (1, 'tibia', 'test@test.com', 0, 0, 0);

INSERT IGNORE INTO `players` (
  `id`, `name`, `account_id`, `group_id`, `sex`, `vocation`,
  `experience`, `level`, `health`, `healthmax`, `mana`, `manamax`,
  `lookbody`, `lookfeet`, `lookhead`, `looklegs`, `looktype`,
  `posx`, `posy`, `posz`, `cap`, `save`
) VALUES (
  1, 'Ps2 Tester', 1, 5, 1, 1,
  4200, 8, 0, 185, 185, 35, 35,
  20, 40, 10, 30, 129,
  996, 1000, 7, 470, 1
) ON DUPLICATE KEY UPDATE `name` = 'Ps2 Tester';