
-- ANCIENT

UPDATE Buildings SET Cost = '65' WHERE Type IN 
('BUILDING_GRANARY',
'BUILDING_YURT',
'BUILDING_MONUMENT',
'BUILDING_MAYA_PYRAMID',
'BUILDING_SHRINE',
'BUILDING_STELE',
'BUILDING_WELL',
'BUILDING_GROVE');

UPDATE Buildings SET Cost = '110' WHERE Type IN 
('BUILDING_HERBALIST',
'BUILDING_BARRACKS',
'BUILDING_IKANDA',
'BUILDING_BAZAAR',
'BUILDING_MARKET',
'BUILDING_STONE_WORKS',
'BUILDING_LONGHOUSE',
'BUILDING_MUD_PYRAMID_MOSQUE',
'BUILDING_WALLS',
'BUILDING_WALLS_OF_BABYLON');


UPDATE Buildings SET GoldMaintenance = '0' WHERE Type IN 
('BUILDING_SHRINE',
'BUILDING_MAYA_PYRAMID',
'BUILDING_GROVE',
'BUILDING_MONUMENT',
'BUILDING_STELE',
'BUILDING_YURT');

UPDATE Buildings SET GoldMaintenance = '1' WHERE Type IN 
('BUILDING_BARRACKS',
'BUILDING_IKANDA',
'BUILDING_GRANARY',
'BUILDING_STONE_WORKS',
'BUILDING_LONGHOUSE',
'BUILDING_MUD_PYRAMID_MOSQUE',
'BUILDING_WALLS',
'BUILDING_WALLS_OF_BABYLON',
'BUILDING_WELL',
'BUILDING_HERBALIST');

-- CLASSICAL
UPDATE Buildings SET Cost = '150' WHERE Type IN 
('BUILDING_FLOATING_GARDENS',
'BUILDING_COLOSSEUM',
'BUILDING_FLAVIAN_COLOSSEUM',
'BUILDING_FORGE',
'BUILDING_SIEGE_WORKSHOP',
'BUILDING_LIBRARY',
'BUILDING_PAPER_MAKER',
'BUILDING_LIGHTHOUSE',
'BUILDING_JELLING_STONES',
'BUILDING_ROYAL_LIBRARY',
'BUILDING_WATERMILL');

UPDATE Buildings SET Cost = '200' WHERE Type IN 
('BUILDING_AMPHITHEATER',
'BUILDING_ODEON',
'BUILDING_WRITERS_GUILD',
'BUILDING_AQUEDUCT',
'BUILDING_INDUS_CANAL',
'BUILDING_BURIAL_TOMB',
'BUILDING_CARAVANSARY',
'BUILDING_COURTHOUSE',
'BUILDING_SATRAPS_COURT',
'BUILDING_TEMPLE',
'BUILDING_BASILICA',
'BUILDING_BATH');


UPDATE Buildings SET GoldMaintenance = '0' WHERE Type = 'BUILDING_CARAVANSARY';
UPDATE Buildings SET GoldMaintenance = '0' WHERE Type = 'BUILDING_BURIAL_TOMB';

UPDATE Buildings SET GoldMaintenance = '1' WHERE Type IN 
('BUILDING_AMPHITHEATER',
'BUILDING_ODEON',
'BUILDING_AQUEDUCT',
'BUILDING_INDUS_CANAL',
'BUILDING_COLOSSEUM',
'BUILDING_FLAVIAN_COLOSSEUM',
'BUILDING_FLOATING_GARDENS',
'BUILDING_FORGE',
'BUILDING_SIEGE_WORKSHOP',
'BUILDING_LIBRARY',
'BUILDING_PAPER_MAKER',
'BUILDING_LIGHTHOUSE',
'BUILDING_JELLING_STONES',
'BUILDING_ROYAL_LIBRARY',
'BUILDING_TEMPLE',
'BUILDING_BASILICA',
'BUILDING_BATH',
'BUILDING_WATERMILL',
'BUILDING_WRITERS_GUILD');

UPDATE Buildings SET GoldMaintenance = '2' WHERE Type = 'BUILDING_SATRAPS_COURT';
UPDATE Buildings SET GoldMaintenance = '3' WHERE Type = 'BUILDING_COURTHOUSE';

UPDATE Buildings SET FaithCost = '200' WHERE Type = 'BUILDING_AMPHITHEATER';
UPDATE Buildings SET FaithCost = '200' WHERE Type = 'BUILDING_ODEON';

-- MEDIEVAL
--UPDATE Buildings SET Cost = '275' WHERE Type = 'BUILDING_MISSION';

UPDATE Buildings SET Cost = '300' WHERE Type IN 
('BUILDING_ARMORY',
'BUILDING_DOJO',
'BUILDING_CANDI',
'BUILDING_CASTLE',
'BUILDING_STABLE',
'BUILDING_DUCAL_STABLE',
'BUILDING_CEILIDH_HALL',
'BUILDING_CIRCUS',
'BUILDING_GARDEN',
'BUILDING_UNIVERSITY',
'BUILDING_SEOWON');

UPDATE Buildings SET Cost = '350' WHERE Type IN 
('BUILDING_WAT',
'BUILDING_HARBOR',
'BUILDING_MINT',
'BUILDING_HANSE',
'BUILDING_WORKSHOP',
'BUILDING_ARTISTS_GUILD');

UPDATE Buildings SET GoldMaintenance = '0' WHERE Type = 'BUILDING_MINT';
UPDATE Buildings SET GoldMaintenance = '0' WHERE Type = 'BUILDING_HANSE';

UPDATE Buildings SET GoldMaintenance = '2' WHERE Type IN 
('BUILDING_ARMORY',
'BUILDING_DOJO',
'BUILDING_CANDI',
'BUILDING_CASTLE',
'BUILDING_STABLE',
'BUILDING_DUCAL_STABLE',
'BUILDING_CEILIDH_HALL',
'BUILDING_CIRCUS',
'BUILDING_HARBOR',
'BUILDING_UNIVERSITY',
'BUILDING_SEOWON',
'BUILDING_WORKSHOP',
'BUILDING_ARTISTS_GUILD',
'BUILDING_WAT');

UPDATE Buildings SET GoldMaintenance = '3' WHERE Type = 'BUILDING_GARDEN';

--UPDATE Buildings SET GoldMaintenance = '0' WHERE Type = 'BUILDING_MISSION';

UPDATE Buildings SET FaithCost = '0' WHERE Type = 'BUILDING_CEILIDH_HALL';
UPDATE Buildings SET FaithCost = '0' WHERE Type = 'BUILDING_WAT';

UPDATE Buildings SET FaithCost = '300' WHERE Type = 'BUILDING_UNIVERSITY';
UPDATE Buildings SET FaithCost = '300' WHERE Type = 'BUILDING_SEOWON';

-- RENAISSANCE
UPDATE Buildings SET Cost = '500' WHERE Type IN 
('BUILDING_BANK',
'BUILDING_COFFEE_HOUSE',
'BUILDING_CONSTABLE',
'BUILDING_OBSERVATORY',
'BUILDING_GROCER');

UPDATE Buildings SET Cost = '600' WHERE Type IN 
('BUILDING_OPERA_HOUSE',
'BUILDING_WINDMILL',
'BUILDING_MUSICIANS_GUILD');

UPDATE Buildings SET GoldMaintenance = '4' WHERE Type IN 
('BUILDING_COFFEE_HOUSE',
'BUILDING_CONSTABLE',
'BUILDING_OBSERVATORY',
'BUILDING_OPERA_HOUSE',
'BUILDING_WINDMILL',
'BUILDING_MUSICIANS_GUILD',
'BUILDING_GROCER');

UPDATE Buildings SET FaithCost = '350' WHERE Type = 'BUILDING_OPERA_HOUSE';

-- INDUSTRIAL
UPDATE Buildings SET Cost = '800' WHERE Type = 'BUILDING_KREPOST';
UPDATE Buildings SET Cost = '800' WHERE Type = 'BUILDING_SKOLA';

UPDATE Buildings SET Cost = '1000' WHERE Type IN 
('BUILDING_ARSENAL',
'BUILDING_HOTEL',
'BUILDING_PUBLIC_SCHOOL',
'BUILDING_THEATRE');

UPDATE Buildings SET Cost = '1250' WHERE Type IN 
('BUILDING_FACTORY',
'BUILDING_MILITARY_ACADEMY',
'BUILDING_STOCKYARD',
'BUILDING_MUSEUM',
'BUILDING_TRAINSTATION');

UPDATE Buildings SET Cost = '1500' WHERE Type = 'BUILDING_SEAPORT';

UPDATE Buildings SET GoldMaintenance = '5' WHERE Type = 'BUILDING_KREPOST';
UPDATE Buildings SET GoldMaintenance = '5' WHERE Type = 'BUILDING_SKOLA';


UPDATE Buildings SET GoldMaintenance = '6' WHERE Type IN 
('BUILDING_HOTEL',
'BUILDING_MILITARY_ACADEMY',
'BUILDING_MUSEUM',
'BUILDING_PUBLIC_SCHOOL',
'BUILDING_THEATRE',
'BUILDING_ARSENAL');

UPDATE Buildings SET GoldMaintenance = '7' WHERE Type IN 
('BUILDING_STOCKYARD',
'BUILDING_FACTORY',
'BUILDING_TRAINSTATION',
'BUILDING_SEAPORT');

UPDATE Buildings SET FaithCost = '400' WHERE Type = 'BUILDING_MUSEUM';
UPDATE Buildings SET FaithCost = '400' WHERE Type = 'BUILDING_PUBLIC_SCHOOL';

-- MODERN

UPDATE Buildings SET Cost = '1600' WHERE Type = 'BUILDING_MINEFIELD';

UPDATE Buildings SET Cost = '1800' WHERE Type IN 
('BUILDING_HOSPITAL',
'BUILDING_COAL_PLANT',
'BUILDING_STOCK_EXCHANGE');

UPDATE Buildings SET Cost = '2000' WHERE Type = 'BUILDING_LABORATORY';
UPDATE Buildings SET Cost = '2000' WHERE Type = 'BUILDING_BROADCAST_TOWER';

UPDATE Buildings SET GoldMaintenance = '0' WHERE Type = 'BUILDING_STOCK_EXCHANGE';

UPDATE Buildings SET GoldMaintenance = '8' WHERE Type IN 
('BUILDING_MINEFIELD',
'BUILDING_HOSPITAL',
'BUILDING_BROADCAST_TOWER',
'BUILDING_COAL_PLANT',
'BUILDING_LABORATORY');

UPDATE Buildings SET FaithCost = '500' WHERE Type = 'BUILDING_BROADCAST_TOWER';
UPDATE Buildings SET FaithCost = '500' WHERE Type = 'BUILDING_LABORATORY';

-- ATOMIC

UPDATE Buildings SET Cost = '2250' WHERE Type IN 
('BUILDING_MEDICAL_LAB',
'BUILDING_MILITARY_BASE',
'BUILDING_POLICE_STATION',
'BUILDING_STADIUM',
'BUILDING_BOMB_SHELTER',
'BUILDING_RECYCLING_CENTER',
'BUILDING_AIRPORT');

UPDATE Buildings SET GoldMaintenance = '10' WHERE Type IN 
('BUILDING_BOMB_SHELTER',
'BUILDING_MEDICAL_LAB',
'BUILDING_MILITARY_BASE',
'BUILDING_POLICE_STATION',
'BUILDING_STADIUM',
'BUILDING_RECYCLING_CENTER',
'BUILDING_AIRPORT');

UPDATE Buildings SET FaithCost = '600' WHERE Type = 'BUILDING_STADIUM';

-- INFORMATION

UPDATE Buildings SET Cost = '2250' WHERE Type = 'BUILDING_SOLAR_PLANT';
UPDATE Buildings SET Cost = '2500' WHERE Type IN 
('BUILDING_NUCLEAR_PLANT',
'BUILDING_WIND_PLANT',
'BUILDING_HYDRO_PLANT');

UPDATE Buildings SET Cost = '2750' WHERE Type = 'BUILDING_SPACESHIP_FACTORY';

UPDATE Buildings SET GoldMaintenance = '10' WHERE Type IN 
('BUILDING_HYDRO_PLANT',
'BUILDING_WIND_PLANT',
'BUILDING_NUCLEAR_PLANT',
'BUILDING_SOLAR_PLANT',
'BUILDING_SPACESHIP_FACTORY');

