# Keys of the characters in the game's own maps (static scan, 2026-11-18)

Source: the 39 map files (`maps/*.d3dbsp`) in the player's own iwd archives; the entity text of every map was read (read-only) and the
`actor_*` spawners were counted by `script_noteworthy` (the key our kill-by-key uses: a spawned character inherits it) and by `targetname`
(NOT inherited by the character: the sniper of downtown_sniper has `targetname=-`).
- "single" = the key occurs on exactly ONE spawner of that map: a candidate for a single named character.
- "shared" = several spawners carry it: a crowd or a group (not a candidate by itself; one random choice among them is the downtown_sniper case:
  the five windows `building1_floor1/2/3`, `building2_floor1/3` are shared x2 each and are folded into the key `enemy_sniper` in make_mod.ps1).
A single key is only a CANDIDATE: whether the character matters for an objective is decided by the mission script (waittill "death", level.<name> =
spawned character); see the script scan in docs/NPC_SYNC.md. Nothing here was run in the game.

Script scan (single characters kept in `level.<name>` after `waittill("spawned")`): cityhall volsky; decoytown macgregor, price; demolition trenchFollowGuy;
downtown_sniper soldier1, soldier4, volsky (+ a random spawner pick); duhoc_defend braeburn, randall; eldaba doorbreachmgguy, doorkicker, lastguy;
matmata britfour, britthree, driver; moscow smoke_guy; tankhunt volsky; credits axisDoorKicker; trainyard has 2 random picks.

== 88ridge: 45 actor spawners; script_noteworthy keys: 0 single, 0 shared; targetnames: 1 single
   single script_noteworthy: 
   shared script_noteworthy: 
   single targetname: auto3874
== beltot: 129 actor spawners; script_noteworthy keys: 3 single, 6 shared; targetnames: 4 single
   single script_noteworthy: 5, price, tmp_mcgregor
   shared script_noteworthy: 4 x2, crouch x2, final_building x7, huntplayerguy x20, mortar_prop_1 x2, mortar_prop_2 x2
   single targetname: axis_mg42_nest_guy, mcgregor, surrender_guy, surrender_guy_2
== bergstein: 146 actor spawners; script_noteworthy keys: 8 single, 5 shared; targetnames: 5 single
   single script_noteworthy: corner_guy, randall, ranger1, ranger2, ranger3, ranger4, ranger5, ranger6
   shared script_noteworthy: easthp_upstairs x4, house1_upstairs x3, house2_upstairs x3, house3_upstairs x2, house4_upstairs x4
   single targetname: house_mg42_gunner, panzer_guy, panzer_support_1, panzer_support_2, zombie
== breakout: 157 actor spawners; script_noteworthy keys: 3 single, 10 shared; targetnames: 2 single
   single script_noteworthy: ht_gunner, mcgregor, price
   shared script_noteworthy: attack_wave01 x3, attack_wave02 x3, barn_guys x3, fieldhq_guys x8, first_mortar_crew x5, guys x14, house01guy x8, second_mortar_crew x5, sortie x35, tank_defenders x4
   single targetname: auto907, auto946
== cityhall: 43 actor spawners; script_noteworthy keys: 0 single, 0 shared; targetnames: 4 single
   single script_noteworthy: 
   shared script_noteworthy: 
   single targetname: auto64, auto66, auto91, volsky
== credits: 82 actor spawners; script_noteworthy keys: 1 single, 3 shared; targetnames: 24 single
   single script_noteworthy: axis_door_kick_spawner
   shared script_noteworthy: halftrack_rider x4, jeep_guy x4, mortar_spawner x2
   single targetname: auto382, cal30_smoke_spawner, car_spawner, door_spawner, driver_spawner, ender, ender_jeep_spawner, ender_postjeep_spawner, ender_smoke_spawner, gunner_spawner, mg_spawner, mgjump_spawner, mortar_spawner, pacifist_spawner, price, price_end_spawner, price_smoke_spawner, prison_guard, randall_end_spawner, redshirt_smoke_spawner, redshirt_spawner, redshirt_target_practice_spawner, sneaky_buddy_spawner, staircase_spawner
== crossroads: 162 actor spawners; script_noteworthy keys: 9 single, 1 shared; targetnames: 6 single
   single script_noteworthy: corner_guy, intro_wall_stacker1, intro_wall_stacker2, panzer_guy, panzer_guy02, price, window_guy_01, window_guy_02, window_guy_03
   shared script_noteworthy: window_runner x2
   single targetname: barn_wave_mg42_guy1, barn_wave_mg42_guy2, farmhouse_spawner_mg42, mg42_guys1_gunner, mg42_guys3_gunner, reinforce1
== decoytown: 143 actor spawners; script_noteworthy keys: 0 single, 2 shared; targetnames: 2 single
   single script_noteworthy: 
   shared script_noteworthy: gotolayernode x6, panzerfaustguy x4
   single targetname: macgregor, price
== decoytrenches: 133 actor spawners; script_noteworthy keys: 2 single, 10 shared; targetnames: 16 single
   single script_noteworthy: delete_by_spawnflags, mg42_blindguy
   shared script_noteworthy: bunker one ai x5, bunker two ai x3, distraction x4, emplacement one ai x4, flanker_front x4, flanker_left x2, runner x5, soldier3 x3, soldier4 x6, trench fodder x5
   single targetname: auto2344, auto2386, auto2389, auto2390, auto2391, auto2392, auto2434, auto2439, auto2444, auto2474, auto30, auto8023, macgregor, price, soldier1, soldier4intro
== demolition: 141 actor spawners; script_noteworthy keys: 7 single, 6 shared; targetnames: 5 single
   single script_noteworthy: barricade_left, barricade_right, call_engineers, plant_explosives, question, smgguy, zero_accuracy
   shared script_noteworthy: axis_defend x3, engineer x3, rifle_spawner x4, rifleguy x5, smg_spawner x3, waver x5
   single targetname: commissar, defenders, letlev, player_killer, trench_follow_guy
== downtown_assault: 104 actor spawners; script_noteworthy keys: 5 single, 5 shared; targetnames: 9 single
   single script_noteworthy: dont_kill_me, rifle_spawner, smg_spawner, smgguy, wave
   shared script_noteworthy: ally_die x5, disable_goal x6, objective_5_axis x14, player_enters_attacker x3, rifleguy x4
   single targetname: auto2170, auto2211, auto2215, auto2249, crazy_guy, crazy_killers, depot_attacker, german_preattackers, volsky
== downtown_sniper: 74 actor spawners; script_noteworthy keys: 3 single, 7 shared; targetnames: 4 single
   single script_noteworthy: helmetguy, spotterguy, warningguy
   shared script_noteworthy: building1_floor1 x2, building1_floor2 x2, building1_floor3 x2, building2_floor1 x2, building2_floor3 x2, retreating_guys x6, scattersoldier x3
   single targetname: auto131, auto152, auto153, ge_tank_commander
== duhoc_assault: 347 actor spawners; script_noteworthy keys: 8 single, 24 shared; targetnames: 15 single
   single script_noteworthy: cal30_dies, cal30gunner, carter, clifftop_mg42_gunner, ignoreme, mccloskey, medic, ward
   shared script_noteworthy: badass_temp x4, blind_mg_gunner x2, bulletshield x7, bunker_friends x10, cliff_fighter x4, cliff_grenade_drop x4, delete_for_minspec x25, escape x8, field_german_spawner x8, friendlychain x6, gate_friendlychain x4, gate_node_1 x3, gate_node_2 x2, house_defenders x2, lowhealth x6, player_squad x3, playerseek x5, slowmovement x4, small_goal_until_pain x4, smallgoalfirst x5, spawn_coupler x3, wait to engage x10, wait to engage delete x3, zombie_guy x2
   single targetname: auto2456, blowupguy_spawner, braeburn_roadblock, drag_guy, flakveirling_crew_spawner, gate_bomb_planter, grenader_targetdummy, pf17369_auto2035, pf17369_auto2041, pf17369_auto2097, pf17369_auto2423, pf17369_auto294, randall_spawner_guns, spawner_gunsnothere_braeburn, zombie
== duhoc_defend: 190 actor spawners; script_noteworthy keys: 2 single, 4 shared; targetnames: 18 single
   single script_noteworthy: 30calguy, in safe area
   shared script_noteworthy: character x2, defend_area x6, orchard_mg_guy x2, sortie x120
   single targetname: auto2550_cut, auto2552_cut, auto2557_cut, auto2565_cut, auto2631, axis_doorkicker, braeburn, charlie_assault_s_1_cut, charlie_assault_s_2, charlie_assault_s_2_cut, charlie_assault_s_3, charlie_assault_s_3_cut, charlie_assault_s_4_cut, charlie_assault_s_6, charlie_assault_s_6_cut, charlie_assault_s_7_cut, farmarea_axis_1_cut, farmdoor_guy
== elalamein: 160 actor spawners; script_noteworthy keys: 0 single, 0 shared; targetnames: 6 single
   single script_noteworthy: 
   shared script_noteworthy: 
   single targetname: NOTauto3279, auto2452, auto2620, auto2686, auto3056, price
== eldaba: 179 actor spawners; script_noteworthy keys: 13 single, 17 shared; targetnames: 13 single
   single script_noteworthy: 0, 2, 3, 4, cynical, door_kicker, macgregor, mg42_artillery_roof, one, soldier1, soldier2, soldier3, two
   shared script_noteworthy: 1 x5, crate_walkers x2, dock_last_ditch_guys x5, end_leftflank_node x6, intro_2ndfloor x2, mg42balcony_guys x2, mg42nest_guys x5, mgnest_dummies x7, no_delete x10, reinforcements x2, reinforcements3 x5, reinforcements4 x5, reinforcements5 x2, reinforcements6 x2, reinforcements7 x2, reinforcements8 x2, scripted_intro_allies x4
   single targetname: auto7080, auto8394, auto8395, auto8398, auto8401, director, end_lastenemy, end_waver, endmission_mac, endmission_price, fc600, reinforcements0, reinforcements_old_0
== hill400_assault: 141 actor spawners; script_noteworthy keys: 1 single, 8 shared; targetnames: 5 single
   single script_noteworthy: thrower
   shared script_noteworthy: coverleft x4, coverright x6, first_bunker_crew x4, forth_bunker_crew x5, mortar01_crew x3, mortar02_crew x2, second_bunker_crew x5, third_bunker_crew x6
   single targetname: braeburn, gr1, mccloskey, myers, randall
== hill400_defend: 88 actor spawners; script_noteworthy keys: 1 single, 2 shared; targetnames: 13 single
   single script_noteworthy: 30calguy
   shared script_noteworthy: assassin x16, mortarguy x11
   single targetname: Cpl. Smith, auto156, auto157, auto158, auto159, auto160, braeburn, carter, flood_spawner, mccloskey, mccloskey_endspawner, randall, randall_endspawner
== libya: 7 actor spawners; script_noteworthy keys: 0 single, 0 shared; targetnames: 0 single
   single script_noteworthy: 
   shared script_noteworthy: 
   single targetname: 
== matmata: 208 actor spawners; script_noteworthy keys: 8 single, 0 shared; targetnames: 19 single
   single script_noteworthy: 1, 2, 3, 4, 5, macgregor, naziTrucker, price
   shared script_noteworthy: 
   single targetname: auto2577, auto2607, auto2608, brit3, brit4, driver, fourthPanzer, gateshaker, gunner, leftloader, macgregor, macgregor2, price2, rightloader, secondPanzer, smokeRunners, smokeThrower01, smokeyTheBrit, thirdPanzer
== moscow: 67 actor spawners; script_noteworthy keys: 6 single, 9 shared; targetnames: 5 single
   single script_noteworthy: attack_player, basement_leader, informer2, intro_driver, letlev, letlev2
   shared script_noteworthy: basementliver x2, house1_bof x4, house1_runners x4, ht1_guys x4, intro_potato_guys x2, intro_shooters x4, last_guys x5, min_spec x4, reinforcements x6
   single targetname: informer, informer2, prison_gaurd, prison_german, sandbags_blow_guy
== newvillers: 251 actor spawners; script_noteworthy keys: 3 single, 10 shared; targetnames: 27 single
   single script_noteworthy: attack, grenade, smg_spawner
   shared script_noteworthy: delete x16, hard_only x8, hide x8, interval x4, player_attack x13, rifle_spawner x4, rifleguy x4, smgguy x2, townhall x2, townhall2 x7
   single targetname: auto1103, auto1113, auto1114, auto1115, auto1168, auto1182, auto1185, auto1206, auto1207, auto1220, auto1222, auto1223, auto1224, auto1225, auto1298, auto1299, auto1300, auto1324, auto1371, auto1482, auto1483, auto1492, auto1548, auto1549, auto1663, auto1671, auto1720
== rhine: 344 actor spawners; script_noteworthy keys: 1 single, 7 shared; targetnames: 1 single
   single script_noteworthy: blake
   shared script_noteworthy: contained x2, fallback x43, mccloskey x2, my_guy x8, randall x2, retreat x16, sortie x67
   single targetname: auto2914
== silotown_assault: 246 actor spawners; script_noteworthy keys: 2 single, 21 shared; targetnames: 17 single
   single script_noteworthy: Pvt. McCloskey returns, escapenode_barnroof
   shared script_noteworthy: barnbags x2, barnbldg x6, buddy_farmalley x4, buddy_field x4, buddy_firsthouse x4, buddy_startcorner x4, buddy_startroad x4, escapenode_groundlevel x21, farmbldg x4, field_flak_crew x2, flanker x8, forkbldg x4, hq_sandbags x2, hqbldg x6, mortar_carrier x14, officebags x2, officebldg x4, officewall x2, opportunity_guys x3, signbldg x4, signwall x2
   single targetname: auto802, barnroof_defender_sniper, braeburn, doc, garcia, hawkins, jones, leftgap_defender_mg42, leftgap_defender_sniper, lentz, mccloskey, nearwindow_defender_mg42, nearwindow_defender_sniper, randall, rightgap_defender_mg42, rightgap_defender_sniper, rosetti
== tankhunt: 207 actor spawners; script_noteworthy keys: 2 single, 5 shared; targetnames: 3 single
   single script_noteworthy: volsky, volsky2
   shared script_noteworthy: contained x6, roadblock_guy x3, sortie x51, square_allies x6, square_axis x10
   single targetname: auto2307, auto2868, piano_tumble_guy
== toujane: 221 actor spawners; script_noteworthy keys: 2 single, 8 shared; targetnames: 6 single
   single script_noteworthy: soldier1, soldier2
   shared script_noteworthy: attackplayer x38, battleshowguys x2, gunner x2, lead tank rider x6, macgregor_mosque x2, roofjumper x3, smallgoalfirst x25, walk_chain_then_attack_player x14
   single targetname: auto2089, auto2294, auto2405, auto2406, auto2408, ladder_guy1
== toujane_ride: 214 actor spawners; script_noteworthy keys: 7 single, 8 shared; targetnames: 7 single
   single script_noteworthy: 3, macgregor, macgregor2, mg42_guy, price, price2, waver
   shared script_noteworthy: 0 x2, 1 x2, 2 x3, delayed_runaway_delete_at_goal x3, delete_at_goal x11, delete_at_goal_nosight x24, no_damage_from_AI x11, smallgoalfirst x3
   single targetname: armored_car_gunner_spawner, auto8603, auto8605, auto8816, auto8874, auto8876, smokeThrower
== trainyard: 179 actor spawners; script_noteworthy keys: 1 single, 8 shared; targetnames: 4 single
   single script_noteworthy: halftrack_officer
   shared script_noteworthy: charge_soldier x4, halftrack_soldier x5, mortar_prop_1 x2, mortar_prop_2 x2, mortar_prop_3 x2, mortar_prop_4 x2, trench_soldier x2, trenchfriendlywave_soldier x8
   single targetname: barrel_officer, pipe_closing_russian, pipe_russian_front, pipe_russian_rear