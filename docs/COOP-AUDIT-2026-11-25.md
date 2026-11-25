# Инвентаризация STM — 25.11.2026

Срез [f630d04](https://github.com/Mobelon6/skyrim.together/tree/f630d04bd9005d49f554a7e9e40cca4847c5f831), development/coop-dialogue-recovery. Это **результат аудита**, не второй реестр задач. Действующие задачи, размеры, порядок и статус — [COOP-STATUS.md](COOP-STATUS.md); журнал [WORKLOG.md](WORKLOG.md). Ничего не удалено, runtime не менялся.

## Метод и ограничения

Проверены дерево репозитория, действующие требования/спецификации, история и workflow. Сопоставлены полные цепочки check/build (65 основных патчей, совпадают), ещё 2 патча submodule, 57 вызовов run_check, 30 из них по именам home/guest/estate. 42 из 65 основных патчей имеют префикс Home. 67 файлов .patch распределены ниже без пропусков/дубликатов по преимущественному назначению. Смешанный патч не равен одному модулю: маркировка не разрешает удалить файл целиком. Проверены добавленные/изменённые пути и участки кода, важные для классификации; выполнена реконструкция конкретных итоговых файлов QuestService.cpp (клиент/сервер), серверного QuestJournal.cpp, DialogueMarker.cpp и DialogueObserver.cpp из закреплённого upstream и обнаруженных изменяющих патчей, с точным совпадением контекста hunks. Прочитаны production ветви follower, shared property, движение, ledger/контейнер, checkpoint, и релевантные тесты. Это предметный статический аудит, не сплошная проверка каждой строки 67 патчей. Прямой git clone в контейнере недоступен (DNS github.com); использован подключённый GitHub. Новая компиляция/игровой запуск не проводились. Последний фактический CI [45680221477](https://github.com/Mobelon6/skyrim.together/actions/runs/45680221477) для 06e861b — check/build success, packaged-ui skipped, публикация ZIP отключена.

## Четыре категории

### A. Лишнее — маловероятно понадобится (23 патчей по основному назначению)

Отменённая архитектура: отдельные физические CELL/клоны, меню выбора личных домов, owner-specific scope/guest lease, проецирование NPC, временная подмена REFR, восстановление оригинального состояния и специальный ESS гостя, переходы/ACL между приватными комнатами/поместьями. Функционал не является целью STM. Не дорабатывать ради завершения.

- HomeInstances.patch
- HomeProjection.patch
- HomeAccessMenu.patch
- HomeVisit.patch
- HomeActorRouting.patch
- HomeResidentProjection.patch
- HomeOwnerRecovery.patch
- HomeLegacyPlacement.patch
- HomeResidentRecovery.patch
- HomeResidentPresence.patch
- HomeGuestDoors.patch
- HomeOriginalOverlay.patch
- HomeOriginalVisit.patch
- HomeGuestSave.patch
- HomeNativeLoose.patch
- HomeNativeDoors.patch
- HomeNativeResidents.patch
- HomeNativeResidentPresence.patch
- HomeLinkedRooms.patch
- HomeLinkedAccess.patch
- HomeLinkedCheckpoint.patch
- HomeEstateRouting.patch
- HomeEstatePermissions.patch

Не всё содержимое этих файлов лишнее: например HomeInstances содержит HomeRegistry и маршруты, HomeProjection — HomePermissions, HomeGuestDoors — изменения HomeObjects; они используются поздними патчами. Извлекать только действительно нужный код, затем прекращать использование приватной ветки. Не удалять файлы или менять формат старых данных вслепую.

Генерация отдельного ESP/CELL/NAVM: tools/build_home_instances.py, tools/home_navmesh.py, tools/package_guest_homes.py; projection/guest-упаковка в tools/build_guest_homes.py (сохранить извлечение метаданных, не генерацию гостевых миров). Тесты таких функций нужны лишь до удаления зависимого runtime; после безопасного исключения не считать их обязательной приёмкой новой игры. Старые инструкции по генерации гостевых интерьеров — архив. Не переносить фантомы (не реализованы) в новый план.

### B. Лишнее сейчас, полезные наработки (9 патчей по основному назначению)

- HomeScope.patch
- HomeSnapshot.patch
- HomeCatalog.patch
- HomeResidents.patch
- HomeActorProtection.patch
- HomeNativeState.patch
- HomeNativeInteractions.patch
- HomeEstateContext.patch
- HomeEstateConstruction.patch

Из них полезны stable plugin/local keys, классификация CELL/REFR/баз, данные каталога Hearthfire, структура снимков, квестовые/alias и enable-зависимости, проверки типов, свойства размещённых объектов/NPC, отдельные native вызовы и guards. Из HomeScope оставить только доказанно нужные обычные проверки ссылок/маршрутов, не личный namespace. HomeEstateContext: ключи и список комнат пригодны, приватный polygon/scope не нужен. HomeEstateConstruction: наблюдение IsDisabled — диагностика, не подтверждение строительной операции. Не развивать bitmap/reversible overlays вместо нативной покупки/строительства. HomeSnapshot и HomeResidents/ActorProtection не переносить целиком без доказанной необходимости.

tools/build_estate_catalog.py, tools/build_native_home_capabilities.py, часть tools/build_guest_homes.py и tools/profile_records.py — полезные offline данные и диагностика; применять только по потребности. Python campaign/ (16 файлов) и отдельный campaign-tests.yml — модели правил, не исполняемая логика STR; сохранить как справочные сценарии, но не тратить время на новый параллельный симулятор или исправление отменённых private-моделей. Документы истории и исследований (COOP-HISTORY, HEARTHFIRE-RESEARCH, NATIVE-HOME-INTERACTIONS, Interaction01, Dialogue02) сохраняют основания; исторические требования не действуют.

### C. Нужное — неотъемлемая утверждённая основа (15 патчей по основному назначению)

- CoopState.patch
- PartyLeadership.patch
- PartyConversationHud.patch
- Dialogue02.patch
- CampaignReplication.patch
- CampaignIdentity.patch
- CampaignCheckpoint.patch
- WorldLifecycle.patch
- ConnectionLifecycle.patch
- SaveLoadLifecycle.patch
- InventoryEvidence.patch
- CampaignTrade.patch
- HomeSpatialEpoch.patch
- OverlayProcess.patch
- TiltedConnectLifecycle.patch

Рабочие общие механизмы, сеть, партийная роль, базовая обработка боя/квестовых сцен и HUD, общий журнал/репликация, идентификация ESS, checkpoint, долговечный ledger и evidence, жизненный цикл подключения/смены мира, spatial epoch. Это требуемые компоненты, не отметка «весь функционал проверен в игре». Не переписывать подтверждённый нелетальный бой. C-патчи также могут включать незавершённые функции/исторические части. Сохранить обязательную документацию (AGENTS, README, DEVELOPMENT, COOP-STATUS, WORKLOG, действующие спецификации), build.yml, лицензии, тесты реального необходимого кода, tools/build_trade_plugin.py. dialogue-ui-test.yml полезен после снятия блокера архива.

### D. Важное — утверждено, но требует завершения (20 патчей по основному назначению)

- PartyTravelAndRecovery.patch
- Interaction02.patch
- Interaction03.patch
- Interaction04.patch
- Interaction05.patch
- Interaction06.patch
- Interaction07.patch
- CampaignTradeAccess.patch
- TradeReturn.patch
- NativeTrade.patch
- TradeItemProperties.patch
- HomeInventoryLedger.patch
- HomeContainerMenu.patch
- HomeLocks.patch
- HomeLooseProperty.patch
- HomeObjectChanges.patch
- HomePlacement.patch
- HomePlacementClient.patch
- HomeSeatLeases.patch
- HomeSharedProperty.patch

Спутники, диалог/наблюдение, перемещения и сцены, предметные операции, нативный обмен/варианты, общие контейнеры, замки/изменения объектов/размещённые вещи, аренда/занятость мебели и переход на общую недвижимость. Их сохранять и доводить до сценария. Из старого home-кода переиспользовать ledger, access, pose/placement, inventory adapter и seat primitives там, где совместимо с общим физическим пространством, не приватную модель. Квестовые предметы/полный общий прогресс, души/слова, radiant/триггеры и восстановление эффекта — также D, хотя отдельного завершённого патча по ним нет. Реестр включает ранее не выделенные утверждённые области общей спецификации: мировой лут и reset, торговцы/крафт, двери/рычаги, обычный бой/AI, преступления, время/сон, личные превращения/магия и совместимость модов. Для последних сначала проверить штатный STR, не объявлять неисправными без основания; неоднозначные предложенные правила (например групповые штрафы) не утверждать от имени пользователя.

**Порядок исключения A:** оставить Git-историю; убрать вызовы отменённого поведения из действующего общего пути и старые артефакты из целевой упаковки только после анализа зависимостей. Сохранить нужные generic компоненты. Проверить compile/CI и фактическую маршрутизацию. Не заниматься масштабным переписыванием/перебазированием 65 патчей как отдельной самоцелью; выводить устаревшее из runtime по мере интеграции актуального функционала. Отдельный архивный код может остаться полезным источником. Не активировать старую private-модель как fallback для обычных домов.

## Обнаруженные ошибки и несоответствия (не список гипотетических дефектов)

| ID | Фактическое основание | Следствие и исправление |
|---|---|---|
| BUG-01 / NPC-01 | [Interaction07.patch](../Interaction07.patch): PartyService::OnPlayerLeave стирает **все** записи m_followers с Owner == leaving, включая Confirmed, посылает FollowerReleased. CoopInteraction дополнительно удаляет запись, если GetPlayerManager().GetById(f.Owner) отсутствует. Owner — connection ID. | Подтверждённое нарушение правила краткого disconnect: подтверждённый найм освобождается при вылете владельца, возможен чужой найм до возвращения. Сохранять confirmed право по устойчивой CampaignIdentity, отделить от временной резервации и AI, при reconnect привязать новый connection. Не удалять на выходе из партии/потере связи; освобождать при подтверждённом увольнении/явном завершении. Реальный симптом на текущей сборке не тестировался. |
| BUG-02 / QUEST-01, QUEST-02, REC-01 | Итоговый server QuestService::OnQuestChanges (CoopState, PartyTravelAndRecovery, Interaction03/04, CampaignReplication) dedup хранит QuestRuns в party, вызывает RecordQuest до назначения Speaker/SharedRun и отправляет live update только WithinSyncRange и ChoiceListeners. RecordQuest в CampaignTradeAccess пишет quest-v1 в party.JournalRows/QuestJournalDisk, не CampaignJournal. OnJournalRequest отдаёт только строки текущей партии; client OnJournalRecord в CampaignReplication только Store и увеличивает m_journalConfirmed; ни replay ни подтверждения игрового эффекта нет. | Пропущенное из-за дальности/disconnect событие не догоняется этим путём, новый party/server не восстанавливает прежний run из authority journal. Сохранить локальное сценическое согласие для личных эффектов; общий подтверждённый результат/участников/ID запуска записывать в существующую кампанию и применять пропущенное на клиентах. Не создавать новый журнал. Это подтверждённая незавершённая интеграция, не утверждение, что все квесты сейчас сломаны. |
| BUG-03 / QUEST-01 | Итоговый client QuestService::OnQuestUpdate вызывает ScriptSetStage и затем безусловно ставит bResult=true для Started/StageUpdate. ApplyNextHeldStage (CoopState.patch) удаляет stage из очереди ДО вызова и при false не возвращает его и не сверяет IsStageReached. | При реальном отказе native SetStage событие считается успешным/теряется; дальнейшая сцена/stop может идти без предпосылки. Отличать уже достигнутую стадию от настоящего отказа, проверить readback и pending fragment, сохранить/повторить безопасно или сообщить о блокере. Не слепой повтор необратимых фрагментов. Отказ движка на конкретном квесте пока не воспроизводился. |
| BUG-04 / HOME-01 (только если наблюдатель сохраняется) | [HomeSharedProperty.patch](../HomeSharedProperty.patch), ObserveEstateConstruction: Send=true устанавливает Pending, сервер ObserveShared возвращает результат без отдельного ответа, клиент ждёт изменения prior.Revision. Если отправка принята транспортом, но отклонена сервером (например, несовпадение location epoch), а локальное состояние стало другим, условие Pending != bits возвращает управление каждый цикл; прежний запрос уже не повторяется, revision не меняется. | Возможна зависшая публикация до сброса track при выходе/переподключении. Не наращивать отдельный bitmap-протокол: если диагностика нужна, добавить конечный отказ/сверку/повтор, иначе вывести наблюдатель из обязательного runtime. Не принимать наблюдение за строительную квитанцию. Условный статический путь, без воспроизведения в Skyrim. |

Отдельное подтверждённое **исправление прежних записей**: итоговый DialogueObserver уже использует lookScale=.30f только для ручного AngleDelta; DrawDialogueMarker сразу возвращается при ObservingDialogue(); Interaction07 добавил fallback линий, если SceneDepth/MarkerDepth::Begin не удались, и копирование depth resource в MarkerDepth. tools/check_observer.py проверяет .03 из .1 и автослежение; tools/check_dialogue_draw.py — fallback и скрытие в наблюдении. Не заново реализовывать эти изменения. Окклюзия с Community Shaders и освобождение камеры в реальной игре не проверены; fallback линии без глубины не доказывают правильное перекрытие.

**Не считать дополнительными найденными багами:** недоступный ZIP — явный блокер лицензирования; прежний краш MQ101 не доказан исправленным, но и текущая регрессия не доказана. Души пропускаются общим ActorValue обработчиком намеренно — отсутствует подтверждённая отдельная выдача, не исправлять удалением фильтра. Общая недвижимость сейчас лишь экспериментальный флаг с частичной записью; отсутствие native replay, общего адреса контейнера, подтверждения стройки и назначения — известная незавершённость, не новая ошибка. Прежний флаг приватных домов/наличие старого ESP не считать нормальным способом запуска будущей сборки. TPTests теперь проходит — не открывать старое зависание заново. Остальные системы не объявлять безошибочными: полный runtime/game test отсутствует.

## Размер задач

Размер — ориентировочная сложность **полного игрового результата**, не число строк или обещанный срок. Маленькая: локализованная и преимущественно готовая; средняя: несколько уже существующих механизмов, требуется соединить и принять сценарий; большая: сквозное изменение мира/персонального эффекта, нативные скрипты, журнал и восстановление либо неизвестная совместимость. По результатам игры классификация может измениться. См. единственную актуальную таблицу в COOP-STATUS. Отдельный CI, исследование, правка строк, упаковка или тест сами по себе не являются реализованным игровым функционалом; REL/ESP/QA — условия выпуска, не считать дополнительными игровыми функциями.
