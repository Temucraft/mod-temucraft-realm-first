-- mod-temucraft-realm-first: its game master commands as RBAC permissions (103901-103905). Each is linked to the
-- stock security role of its old level (193 game master, 192 administrator) and to the matching Temucraft GM tier
-- (1012 Entertainer, 1014 Administrator). Grant or deny them per account with .rbac account grant / deny, or link
-- them to other roles.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` BETWEEN 103901 AND 103949;
DELETE FROM `rbac_permissions` WHERE `id` BETWEEN 103901 AND 103949;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(103901, 'Command: temucraft realmfirst list'),
(103902, 'Command: temucraft realmfirst eligible'),
(103903, 'Command: temucraft realmfirst reset'),
(103904, 'Command: temucraft realmfirst mark'),
(103905, 'Command: temucraft realmfirst reload');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(193, 103901),
(1012, 103901),
(193, 103902),
(1012, 103902),
(192, 103903),
(1014, 103903),
(192, 103904),
(1014, 103904),
(192, 103905),
(1014, 103905);
