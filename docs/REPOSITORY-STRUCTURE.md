# Структура STM

Перенос от [ea01827](https://github.com/Mobelon6/skyrim.together/tree/ea01827d081e8c0e68dd7151a1a106622c34de22) по [аудиту 25.11.2026](COOP-AUDIT-2026-11-25.md). Категория определяет основное назначение файла, не статус отдельной функции или право удалить файл. Действующий реестр задач — [COOP-STATUS](COOP-STATUS.md).

- `required/` — утверждённая основа, сохранять.
- `important/` — утверждённое, требующее завершения/приёмки.
- `reusable/` — исследовательские и потенциально полезные наработки; не развивать без потребности.
- `obsolete/` — отменённый подход к личным физическим инстансам; постепенно исключать из runtime после сохранения нужных зависимостей. Не дорабатывать отменённое поведение.

Внутри: patches/, tests/, tools/. В reusable/campaign/ — Python-модели правил, не игровой runtime. Три workflow остаются в .github/workflows/. Корневые README, AGENTS, DEVELOPMENT, LICENSING и лицензии сохраняют стандартные точки входа. Документация в docs/ остаётся по стабильным адресам: COOP-STATUS, WORKLOG, Coop-Campaign, HOME-CONCEPT и Party-Dialogue — действующие; COOP-HISTORY, COOP-AUDIT, HEARTHFIRE-RESEARCH, NATIVE-HOME-INTERACTIONS, Interaction01 и Dialogue02 — исторические/справочные. Архивные документы относятся к указанным ревизиям; старые пути доступны в Git по этим SHA или через таблицу ниже.

## Зависимости

Сборка по [build.yml](../.github/workflows/build.yml) сохраняет 65 основных патчей в исходном порядке, 2 патча зависимостей и 57 прежних проверок. Патчи не объединены, не удалены, содержимое прежнее. Категория не означает готовность отдельной сборки без остальных категорий. В частности, obsolete/tools/build_home_instances.py предоставляет stable() для генераторов метаданных в reusable/tools; reusable/tools/build_guest_homes.py пока содержит и отменённую генерацию, и полезное чтение записей. Импорты/пути тестов перенаправлены, а не переписаны. После безопасного отказа от этих зависимостей можно удалить ненужные части и проверки; не делать это ради очистки каталогов. Общая недвижимость не становится автоматически готовой в результате переноса.

## Карта переноса

| Старый путь | Новый путь |
|---|---|
| `campaign/carriage.py` | [reusable/campaign/carriage.py](../reusable/campaign/carriage.py) |
| `campaign/dialogue.py` | [reusable/campaign/dialogue.py](../reusable/campaign/dialogue.py) |
| `campaign/followers.py` | [reusable/campaign/followers.py](../reusable/campaign/followers.py) |
| `campaign/homes.py` | [reusable/campaign/homes.py](../reusable/campaign/homes.py) |
| `campaign/reconnect.py` | [reusable/campaign/reconnect.py](../reusable/campaign/reconnect.py) |
| `campaign/replica.py` | [reusable/campaign/replica.py](../reusable/campaign/replica.py) |
| `campaign/save_recovery.py` | [reusable/campaign/save_recovery.py](../reusable/campaign/save_recovery.py) |
| `campaign/test_carriage.py` | [reusable/campaign/test_carriage.py](../reusable/campaign/test_carriage.py) |
| `campaign/test_dialogue.py` | [reusable/campaign/test_dialogue.py](../reusable/campaign/test_dialogue.py) |
| `campaign/test_followers.py` | [reusable/campaign/test_followers.py](../reusable/campaign/test_followers.py) |
| `campaign/test_homes.py` | [reusable/campaign/test_homes.py](../reusable/campaign/test_homes.py) |
| `campaign/test_replica.py` | [reusable/campaign/test_replica.py](../reusable/campaign/test_replica.py) |
| `campaign/test_save_recovery.py` | [reusable/campaign/test_save_recovery.py](../reusable/campaign/test_save_recovery.py) |
| `campaign/test_trade.py` | [reusable/campaign/test_trade.py](../reusable/campaign/test_trade.py) |
| `campaign/test_world_journal.py` | [reusable/campaign/test_world_journal.py](../reusable/campaign/test_world_journal.py) |
| `campaign/trade.py` | [reusable/campaign/trade.py](../reusable/campaign/trade.py) |
| `campaign/world_journal.py` | [reusable/campaign/world_journal.py](../reusable/campaign/world_journal.py) |
| `CampaignCheckpoint.patch` | [required/patches/CampaignCheckpoint.patch](../required/patches/CampaignCheckpoint.patch) |
| `CampaignIdentity.patch` | [required/patches/CampaignIdentity.patch](../required/patches/CampaignIdentity.patch) |
| `CampaignReplication.patch` | [required/patches/CampaignReplication.patch](../required/patches/CampaignReplication.patch) |
| `CampaignTrade.patch` | [required/patches/CampaignTrade.patch](../required/patches/CampaignTrade.patch) |
| `CampaignTradeAccess.patch` | [important/patches/CampaignTradeAccess.patch](../important/patches/CampaignTradeAccess.patch) |
| `check_dialogue2.py` | [required/tests/check_dialogue2.py](../required/tests/check_dialogue2.py) |
| `check_state3.py` | [required/tests/check_state3.py](../required/tests/check_state3.py) |
| `check.py` | [required/tests/check.py](../required/tests/check.py) |
| `ConnectionLifecycle.patch` | [required/patches/ConnectionLifecycle.patch](../required/patches/ConnectionLifecycle.patch) |
| `CoopState.patch` | [required/patches/CoopState.patch](../required/patches/CoopState.patch) |
| `Dialogue02.patch` | [required/patches/Dialogue02.patch](../required/patches/Dialogue02.patch) |
| `HomeAccessMenu.patch` | [obsolete/patches/HomeAccessMenu.patch](../obsolete/patches/HomeAccessMenu.patch) |
| `HomeActorProtection.patch` | [reusable/patches/HomeActorProtection.patch](../reusable/patches/HomeActorProtection.patch) |
| `HomeActorRouting.patch` | [obsolete/patches/HomeActorRouting.patch](../obsolete/patches/HomeActorRouting.patch) |
| `HomeCatalog.patch` | [reusable/patches/HomeCatalog.patch](../reusable/patches/HomeCatalog.patch) |
| `HomeContainerMenu.patch` | [important/patches/HomeContainerMenu.patch](../important/patches/HomeContainerMenu.patch) |
| `HomeEstateConstruction.patch` | [reusable/patches/HomeEstateConstruction.patch](../reusable/patches/HomeEstateConstruction.patch) |
| `HomeEstateContext.patch` | [reusable/patches/HomeEstateContext.patch](../reusable/patches/HomeEstateContext.patch) |
| `HomeEstatePermissions.patch` | [obsolete/patches/HomeEstatePermissions.patch](../obsolete/patches/HomeEstatePermissions.patch) |
| `HomeEstateRouting.patch` | [obsolete/patches/HomeEstateRouting.patch](../obsolete/patches/HomeEstateRouting.patch) |
| `HomeGuestDoors.patch` | [obsolete/patches/HomeGuestDoors.patch](../obsolete/patches/HomeGuestDoors.patch) |
| `HomeGuestSave.patch` | [obsolete/patches/HomeGuestSave.patch](../obsolete/patches/HomeGuestSave.patch) |
| `HomeInstances.patch` | [obsolete/patches/HomeInstances.patch](../obsolete/patches/HomeInstances.patch) |
| `HomeInventoryLedger.patch` | [important/patches/HomeInventoryLedger.patch](../important/patches/HomeInventoryLedger.patch) |
| `HomeLegacyPlacement.patch` | [obsolete/patches/HomeLegacyPlacement.patch](../obsolete/patches/HomeLegacyPlacement.patch) |
| `HomeLinkedAccess.patch` | [obsolete/patches/HomeLinkedAccess.patch](../obsolete/patches/HomeLinkedAccess.patch) |
| `HomeLinkedCheckpoint.patch` | [obsolete/patches/HomeLinkedCheckpoint.patch](../obsolete/patches/HomeLinkedCheckpoint.patch) |
| `HomeLinkedRooms.patch` | [obsolete/patches/HomeLinkedRooms.patch](../obsolete/patches/HomeLinkedRooms.patch) |
| `HomeLocks.patch` | [important/patches/HomeLocks.patch](../important/patches/HomeLocks.patch) |
| `HomeLooseProperty.patch` | [important/patches/HomeLooseProperty.patch](../important/patches/HomeLooseProperty.patch) |
| `HomeNativeDoors.patch` | [obsolete/patches/HomeNativeDoors.patch](../obsolete/patches/HomeNativeDoors.patch) |
| `HomeNativeInteractions.patch` | [reusable/patches/HomeNativeInteractions.patch](../reusable/patches/HomeNativeInteractions.patch) |
| `HomeNativeLoose.patch` | [obsolete/patches/HomeNativeLoose.patch](../obsolete/patches/HomeNativeLoose.patch) |
| `HomeNativeResidentPresence.patch` | [obsolete/patches/HomeNativeResidentPresence.patch](../obsolete/patches/HomeNativeResidentPresence.patch) |
| `HomeNativeResidents.patch` | [obsolete/patches/HomeNativeResidents.patch](../obsolete/patches/HomeNativeResidents.patch) |
| `HomeNativeState.patch` | [reusable/patches/HomeNativeState.patch](../reusable/patches/HomeNativeState.patch) |
| `HomeObjectChanges.patch` | [important/patches/HomeObjectChanges.patch](../important/patches/HomeObjectChanges.patch) |
| `HomeOriginalOverlay.patch` | [obsolete/patches/HomeOriginalOverlay.patch](../obsolete/patches/HomeOriginalOverlay.patch) |
| `HomeOriginalVisit.patch` | [obsolete/patches/HomeOriginalVisit.patch](../obsolete/patches/HomeOriginalVisit.patch) |
| `HomeOwnerRecovery.patch` | [obsolete/patches/HomeOwnerRecovery.patch](../obsolete/patches/HomeOwnerRecovery.patch) |
| `HomePlacement.patch` | [important/patches/HomePlacement.patch](../important/patches/HomePlacement.patch) |
| `HomePlacementClient.patch` | [important/patches/HomePlacementClient.patch](../important/patches/HomePlacementClient.patch) |
| `HomeProjection.patch` | [obsolete/patches/HomeProjection.patch](../obsolete/patches/HomeProjection.patch) |
| `HomeResidentPresence.patch` | [obsolete/patches/HomeResidentPresence.patch](../obsolete/patches/HomeResidentPresence.patch) |
| `HomeResidentProjection.patch` | [obsolete/patches/HomeResidentProjection.patch](../obsolete/patches/HomeResidentProjection.patch) |
| `HomeResidentRecovery.patch` | [obsolete/patches/HomeResidentRecovery.patch](../obsolete/patches/HomeResidentRecovery.patch) |
| `HomeResidents.patch` | [reusable/patches/HomeResidents.patch](../reusable/patches/HomeResidents.patch) |
| `HomeScope.patch` | [reusable/patches/HomeScope.patch](../reusable/patches/HomeScope.patch) |
| `HomeSeatLeases.patch` | [important/patches/HomeSeatLeases.patch](../important/patches/HomeSeatLeases.patch) |
| `HomeSharedProperty.patch` | [important/patches/HomeSharedProperty.patch](../important/patches/HomeSharedProperty.patch) |
| `HomeSnapshot.patch` | [reusable/patches/HomeSnapshot.patch](../reusable/patches/HomeSnapshot.patch) |
| `HomeSpatialEpoch.patch` | [required/patches/HomeSpatialEpoch.patch](../required/patches/HomeSpatialEpoch.patch) |
| `HomeVisit.patch` | [obsolete/patches/HomeVisit.patch](../obsolete/patches/HomeVisit.patch) |
| `Interaction02.patch` | [important/patches/Interaction02.patch](../important/patches/Interaction02.patch) |
| `Interaction03.patch` | [important/patches/Interaction03.patch](../important/patches/Interaction03.patch) |
| `Interaction04.patch` | [important/patches/Interaction04.patch](../important/patches/Interaction04.patch) |
| `Interaction05.patch` | [important/patches/Interaction05.patch](../important/patches/Interaction05.patch) |
| `Interaction06.patch` | [important/patches/Interaction06.patch](../important/patches/Interaction06.patch) |
| `Interaction07.patch` | [important/patches/Interaction07.patch](../important/patches/Interaction07.patch) |
| `InventoryEvidence.patch` | [required/patches/InventoryEvidence.patch](../required/patches/InventoryEvidence.patch) |
| `NativeTrade.patch` | [important/patches/NativeTrade.patch](../important/patches/NativeTrade.patch) |
| `OverlayProcess.patch` | [required/patches/OverlayProcess.patch](../required/patches/OverlayProcess.patch) |
| `PartyConversationHud.patch` | [required/patches/PartyConversationHud.patch](../required/patches/PartyConversationHud.patch) |
| `PartyLeadership.patch` | [required/patches/PartyLeadership.patch](../required/patches/PartyLeadership.patch) |
| `PartyTravelAndRecovery.patch` | [important/patches/PartyTravelAndRecovery.patch](../important/patches/PartyTravelAndRecovery.patch) |
| `SaveLoadLifecycle.patch` | [required/patches/SaveLoadLifecycle.patch](../required/patches/SaveLoadLifecycle.patch) |
| `tests/check_actor_replay.py` | [required/tests/check_actor_replay.py](../required/tests/check_actor_replay.py) |
| `tests/check_campaign_checkpoint.py` | [required/tests/check_campaign_checkpoint.py](../required/tests/check_campaign_checkpoint.py) |
| `tests/check_campaign_identity.py` | [required/tests/check_campaign_identity.py](../required/tests/check_campaign_identity.py) |
| `tests/check_campaign_journal.py` | [required/tests/check_campaign_journal.py](../required/tests/check_campaign_journal.py) |
| `tests/check_campaign_replication.py` | [required/tests/check_campaign_replication.py](../required/tests/check_campaign_replication.py) |
| `tests/check_connection_lifecycle.py` | [required/tests/check_connection_lifecycle.py](../required/tests/check_connection_lifecycle.py) |
| `tests/check_encounter_combat.py` | [required/tests/check_encounter_combat.py](../required/tests/check_encounter_combat.py) |
| `tests/check_encounter_recovery.py` | [important/tests/check_encounter_recovery.py](../important/tests/check_encounter_recovery.py) |
| `tests/check_estate_catalog.py` | [reusable/tests/check_estate_catalog.py](../reusable/tests/check_estate_catalog.py) |
| `tests/check_guest_homes.py` | [obsolete/tests/check_guest_homes.py](../obsolete/tests/check_guest_homes.py) |
| `tests/check_guest_packaging.py` | [obsolete/tests/check_guest_packaging.py](../obsolete/tests/check_guest_packaging.py) |
| `tests/check_home_actor_protection.py` | [reusable/tests/check_home_actor_protection.py](../reusable/tests/check_home_actor_protection.py) |
| `tests/check_home_catalog.py` | [reusable/tests/check_home_catalog.py](../reusable/tests/check_home_catalog.py) |
| `tests/check_home_container_menu.py` | [important/tests/check_home_container_menu.py](../important/tests/check_home_container_menu.py) |
| `tests/check_home_guest_save.py` | [obsolete/tests/check_home_guest_save.py](../obsolete/tests/check_home_guest_save.py) |
| `tests/check_home_instances_generation.py` | [obsolete/tests/check_home_instances_generation.py](../obsolete/tests/check_home_instances_generation.py) |
| `tests/check_home_instances_plugin.py` | [obsolete/tests/check_home_instances_plugin.py](../obsolete/tests/check_home_instances_plugin.py) |
| `tests/check_home_inventory_ledger.py` | [important/tests/check_home_inventory_ledger.py](../important/tests/check_home_inventory_ledger.py) |
| `tests/check_home_linked_access.py` | [important/tests/check_home_linked_access.py](../important/tests/check_home_linked_access.py) |
| `tests/check_home_linked_rooms.py` | [obsolete/tests/check_home_linked_rooms.py](../obsolete/tests/check_home_linked_rooms.py) |
| `tests/check_home_locks.py` | [important/tests/check_home_locks.py](../important/tests/check_home_locks.py) |
| `tests/check_home_loose_property.py` | [important/tests/check_home_loose_property.py](../important/tests/check_home_loose_property.py) |
| `tests/check_home_native_capabilities.py` | [reusable/tests/check_home_native_capabilities.py](../reusable/tests/check_home_native_capabilities.py) |
| `tests/check_home_native_residents.py` | [obsolete/tests/check_home_native_residents.py](../obsolete/tests/check_home_native_residents.py) |
| `tests/check_home_native_state.py` | [reusable/tests/check_home_native_state.py](../reusable/tests/check_home_native_state.py) |
| `tests/check_home_navmesh.py` | [obsolete/tests/check_home_navmesh.py](../obsolete/tests/check_home_navmesh.py) |
| `tests/check_home_objects.py` | [important/tests/check_home_objects.py](../important/tests/check_home_objects.py) |
| `tests/check_home_original_overlay.py` | [obsolete/tests/check_home_original_overlay.py](../obsolete/tests/check_home_original_overlay.py) |
| `tests/check_home_original_visit.py` | [obsolete/tests/check_home_original_visit.py](../obsolete/tests/check_home_original_visit.py) |
| `tests/check_home_placement.py` | [important/tests/check_home_placement.py](../important/tests/check_home_placement.py) |
| `tests/check_home_projection.py` | [obsolete/tests/check_home_projection.py](../obsolete/tests/check_home_projection.py) |
| `tests/check_home_registry.py` | [obsolete/tests/check_home_registry.py](../obsolete/tests/check_home_registry.py) |
| `tests/check_home_resident_projection.py` | [obsolete/tests/check_home_resident_projection.py](../obsolete/tests/check_home_resident_projection.py) |
| `tests/check_home_residents.py` | [reusable/tests/check_home_residents.py](../reusable/tests/check_home_residents.py) |
| `tests/check_home_routes.py` | [obsolete/tests/check_home_routes.py](../obsolete/tests/check_home_routes.py) |
| `tests/check_home_scope.py` | [reusable/tests/check_home_scope.py](../reusable/tests/check_home_scope.py) |
| `tests/check_home_seat_leases.py` | [important/tests/check_home_seat_leases.py](../important/tests/check_home_seat_leases.py) |
| `tests/check_home_snapshot.py` | [reusable/tests/check_home_snapshot.py](../reusable/tests/check_home_snapshot.py) |
| `tests/check_home_spatial_epoch.py` | [required/tests/check_home_spatial_epoch.py](../required/tests/check_home_spatial_epoch.py) |
| `tests/check_home_visit.py` | [obsolete/tests/check_home_visit.py](../obsolete/tests/check_home_visit.py) |
| `tests/check_inventory_evidence.py` | [required/tests/check_inventory_evidence.py](../required/tests/check_inventory_evidence.py) |
| `tests/check_native_trade.py` | [important/tests/check_native_trade.py](../important/tests/check_native_trade.py) |
| `tests/check_party_windows.py` | [important/tests/check_party_windows.py](../important/tests/check_party_windows.py) |
| `tests/check_save_load_lifecycle.py` | [required/tests/check_save_load_lifecycle.py](../required/tests/check_save_load_lifecycle.py) |
| `tests/check_trade.py` | [important/tests/check_trade.py](../important/tests/check_trade.py) |
| `tests/check_world_epoch.py` | [required/tests/check_world_epoch.py](../required/tests/check_world_epoch.py) |
| `TiltedConnectLifecycle.patch` | [required/patches/TiltedConnectLifecycle.patch](../required/patches/TiltedConnectLifecycle.patch) |
| `tools/build_estate_catalog.py` | [reusable/tools/build_estate_catalog.py](../reusable/tools/build_estate_catalog.py) |
| `tools/build_guest_homes.py` | [reusable/tools/build_guest_homes.py](../reusable/tools/build_guest_homes.py) |
| `tools/build_home_instances.py` | [obsolete/tools/build_home_instances.py](../obsolete/tools/build_home_instances.py) |
| `tools/build_native_home_capabilities.py` | [reusable/tools/build_native_home_capabilities.py](../reusable/tools/build_native_home_capabilities.py) |
| `tools/build_trade_plugin.py` | [required/tools/build_trade_plugin.py](../required/tools/build_trade_plugin.py) |
| `tools/check_coop_interaction.py` | [important/tests/check_coop_interaction.py](../important/tests/check_coop_interaction.py) |
| `tools/check_dialogue_draw.py` | [important/tests/check_dialogue_draw.py](../important/tests/check_dialogue_draw.py) |
| `tools/check_dialogue_ui.py` | [important/tests/check_dialogue_ui.py](../important/tests/check_dialogue_ui.py) |
| `tools/check_follower_dialogue.py` | [important/tests/check_follower_dialogue.py](../important/tests/check_follower_dialogue.py) |
| `tools/check_interaction_owner.py` | [important/tests/check_interaction_owner.py](../important/tests/check_interaction_owner.py) |
| `tools/check_life_confirmation.py` | [required/tests/check_life_confirmation.py](../required/tests/check_life_confirmation.py) |
| `tools/check_observer.py` | [important/tests/check_observer.py](../important/tests/check_observer.py) |
| `tools/check_party_travel.py` | [important/tests/check_party_travel.py](../important/tests/check_party_travel.py) |
| `tools/check_progress_consent.py` | [important/tests/check_progress_consent.py](../important/tests/check_progress_consent.py) |
| `tools/check_quest_journal.py` | [required/tests/check_quest_journal.py](../required/tests/check_quest_journal.py) |
| `tools/check_remote_hit.py` | [required/tests/check_remote_hit.py](../required/tests/check_remote_hit.py) |
| `tools/check_replica_and_input.py` | [required/tests/check_replica_and_input.py](../required/tests/check_replica_and_input.py) |
| `tools/home_navmesh.py` | [obsolete/tools/home_navmesh.py](../obsolete/tools/home_navmesh.py) |
| `tools/package_guest_homes.py` | [obsolete/tools/package_guest_homes.py](../obsolete/tools/package_guest_homes.py) |
| `tools/profile_records.py` | [reusable/tools/profile_records.py](../reusable/tools/profile_records.py) |
| `tools/verify_travel_settings.py` | [important/tests/verify_travel_settings.py](../important/tests/verify_travel_settings.py) |
| `TradeItemProperties.patch` | [important/patches/TradeItemProperties.patch](../important/patches/TradeItemProperties.patch) |
| `TradeReturn.patch` | [important/patches/TradeReturn.patch](../important/patches/TradeReturn.patch) |
| `WorldLifecycle.patch` | [required/patches/WorldLifecycle.patch](../required/patches/WorldLifecycle.patch) |
