# Hearthfire — исследование исходных данных после Campaign39 (25.11.2026)

Цель: [HOME-CONCEPT-RU.md](HOME-CONCEPT-RU.md). Это исследование, не готовый каталог и не изменение runtime. Кодовая база 75b2a33, документальное решение 1f7c479.

## Материалы и метод

Фактически материализован архив Library Skyrim-plugins-20261120-204115(1).zip (286530260 байт), прочитан HearthFires.esm и проверены overrides 13 целевых записей по историческим loadorder(20261120-162720).txt и plugins(9).txt. Все активные имена из этих списков найдены в архиве; базовые/CC мастера включены согласно историческому порядку. GRUP recursion, compressed records, XXXX subrecords, MAST-local resolution, VMAD header format 2 и direct object/array properties. Не распаковывались/не проверялись PEX/PSC из BSA, NIF, runtime save or current effective MO2 profile. Исторический экспорт не заменяет финальные winning overrides. Все числовые ID ниже — **plugin-local**, а не текущие load-order FormID.

## 1. Идентификация трёх поместий

| Поместье | QUST HearthFires.esm | Наружный LCTN | Внутренний LCTN | Дом CELL | Подвал CELL | Drafting Table REFR | SiteEnableMarker REFR |
|---|---|---|---|---|---|---|---|
| Falkreath / Lakeview | 00305E | 00308A | 003092 | 002FEC | 004849 | 003063 | 00307B |
| Hjaalmarch / Windstad | 00BE09 | 005209 | 00BE0A | 011CB3 | 015154 | 00A535 | 00A50E |
| Pale / Heljarchen | 010BEE | 00520A | 00BE1D | 016A56 | 016A55 | 00C3B1 | 00C3C6 |

BYOHDraftingTable base FURN HearthFires.esm:00305B имеет VMAD BYOHWorkbenchScript (property BYOHHouseBuilding -> QUST 00305D). Все три столовые REFR имеют XESP -> SiteEnableMarker, flags 0 (без инверсии); сами три маркера в исходном мастере имеют flags 0xC00 (persistent + initially disabled). Исторический профиль: оригинальные столы и base выигрывают, USSEP переопределяет Falkreath marker и все четыре строительные QUST. Falkreath marker сохраняет 0xC00. Windstad стол дополнительно имеет собственный VMAD BYOHWorkbenchScript. Следовательно стол до покупки не гарантированно доступен; само включение SiteEnableMarker изменит связанные объекты и состояние площадки. **Не включать оригинальный marker для меню и не вызывать оригинальную строительную активацию от имени гостя.**

Предпочтительная минимальная реализация: штатный стол, если видим/доступен; при отсутствующем столе — независимая точка доступа на его исходной позиции (подсказка/активация через безопасный proxy либо существующий UI рядом с точкой). Не фиксировать форму proxy до проверки native UI/activation path; не создавать собственный строительный станок. Покупка/стройка остаётся штатной.

## 2. Границы и фактическое размещение

Все три REFR столов и их маркеры хранятся в persistent CELL Skyrim.esm:000D74 мира Tamriel (Skyrim.esm:00003C), а не в соответствующей именованной exterior CELL. Координаты столов (x,y,z, игровые единицы, округление до 0.1):
- Falkreath (-8740.2, -70681.4, 2335.0)
- Hjaalmarch (-23618.1, 97355.0, -13312.3)
- Pale (32337.8, 36690.6, -6900.3)

Named exterior CELL в оригинальном HearthFires.esm (их ключи Skyrim.esm):
- Falkreath: 009BF7 (-3,-18), 009BD6 (-3,-17).
- Hjaalmarch: 0092B2 (-6,23), 009291 (-6,24), 009292 (-7,24), 0092B3 (-7,23).
- Pale: 0090C4 (7,8), 0090A5 (7,9), 0090A4 (8,9).

Из них нельзя получить точную границу участка: persistent refs размещены по мировым координатам; часть зависимостей относится к удалённым объектам. Например, исходный Falkreath marker инверсно управляет Skyrim.esm:08F645/099C60/099C70 (позиции существенно вне строительной площадки), Pale marker — Skyrim.esm:078C59. Это факт связей, не доказательство необходимости менять эти объекты при посещении. Не приватизировать целиком persistent CELL/все связанные exterior cells/всю транзитивную XESP-группу. Раздельно хранить физический world/grid, каталог строительных ссылок, пространственную границу и зависимости. Точную допустимую границу и safe arrival ещё нужно определить; не выбирать произвольный большой радиус по крайним зависимостям.

## 3. Строительные списки и ограничения

В трёх QUST обнаружен byohhousescript с 65 свойствами: EnableList/EnableList2/3/4, DisableList/DisableList2 (224 элемента каждый для основных списков), ExteriorEnableList (9), RoomEnableList/RoomDisableList/RoomStartDisableList, Room01A/B и Room02–12 EnableList, Room01RemodelEnableList/DisableList, мосты/перила, RoomDoneFlags/RoomStartedFlags FLST, SiteEnableMarker, HouseMapMarker, HoldingChests, InteriorSwapTriggers, HouseCraftingTrigger, nav-cut ссылки. VMAD прямые массивы дают существенно более обоснованный seed, чем EDID prefix или весь CELL.

В исходном мастере подсчёт уникальных ненулевых прямых ссылок (alias 65535, существующие в HearthFires.esm, имена свойств содержат EnableList/DisableList):
| QUST | Всего | В Tamriel | Внутренние |
|---|---:|---:|---:|
| Falkreath | 651 | 262 | 389 |
| Hjaalmarch | 651 | 261 | 390 |
| Pale | 650 | 261 | 389 |

В исходном Hjaalmarch DisableList[0] указывает на Falkreath DummyMarker1 003062; DisableList[154] — на Falkreath InteriorWorkbenchRoom12Ref 003B14. Исторический winning USSEP исправляет обе ссылки (00A54E и 0170B2). При разрешении индексов по MAST и подсчёте тех же исходных HearthFires-ключей effective QUST из USSEP каждый содержит 650 уникальных ссылок (261 Tamriel /389 внутренние); у Falkreath ещё 4 вхождения Skyrim.esm:05F2DF в DisableList2 (один уникальный ключ). Подсчёт списков НЕ означает 650 видимых построек и НЕ является полным effective ref catalog: классификация расположения сделана по исходным HearthFires-записям, все дочерние overrides ещё не сведены.

Оригинальные наружные списки включают по 189 ссылок с base STAT из HearthFires.esm и по 11 телепортационных DOOR REFR. Есть отдельные управляющие маркеры, collision/navcut, триггеры, контейнеры, скриптовые объекты и переключаемые двери. Не применять единый визуальный алгоритм ко всем. Для штатных дверей определить реальную взаимность XTEL, состояние выбранного владельца и текущую допустимую точку выхода. Группы строительных списков должны оставаться привязаны к estate, а не к одной CELL; не импортировать локальные GLOB/квестовые/семейные состояния гостю. Наличие enable/disable списков не доказывает, что визуальная подмена без VM безопасна для всех конструкций.

## 4. Фантомы: техническая предпосылка, не проверенный renderer

CommonLibSSE-NG exposes BSModelDB::Demand, NiAVObject::Clone, AttachProperty, UpdateMaterialAlpha:
- https://ng.commonlib.dev/_b_s_model_d_b_8h.html
- https://ng.commonlib.dev/_ni_a_v_object_8h_source.html
- https://github.com/epinter/CommonLibSSE-NG/blob/ng/src/RE/N/NiAVObject.cpp

В опубликованной реализации UpdateMaterialAlpha меняется materialAlpha у материала: простого Clone + изменения alpha нельзя считать гарантией независимости оригинала или корректного blending. Нужны независимые shader/material/alpha properties, исключение Havok/collision/interactive nodes, правильная lifetime/thread и transform/attach/detach, проверка blend/depth/LOD/CS. STR не предоставляет проверенный готовый путь из этих API для указанного runtime. Не добавлять всю CommonLib в STR вслепую. Первичная задача — один стабильный строительный STAT с существующим NIF, без ESP/REFR/физики/VM; отсутствие игровой среды не позволяет подтвердить графику сейчас. Core switching не должен ждать рендеринг.

Дополнительное подтверждение механизма появления площадки (стороннее описание, не проверенный PSC этой сборки): https://www.nexusmods.com/skyrimspecialedition/mods/119448?tab=posts . Оно согласуется с XESP и initially-disabled marker. Точный путь покупки/скрипта и winning PEX ещё проверить.

## 5. Следующая реализация

Новый небольшой read-only estate catalog поверх существующего tools/build_guest_homes.py reader и stable plugin/local keys, **не генератор**. Начать с трёх проверенных QUST/LCTN/столов/markers, автоматически читать actual winning refs/VMAD lists, resolve persistent placement по worldspace и координатам, отделять candidates от подтверждённых safe roots/door pairs. Не подменять все списки без runtime контроля. Сохранить native interior metadata D/S/A/M/L и старые slots. Синтетические тесты плюс прогон на имеющемся историческом профиле; установить/использовать только после финального профиля. Затем привязать estate context к серверному admission/scope и обычной exterior grid, меню стола и безопасному переключению. Сначала обеспечить один сквозной vanilla сценарий, а не универсальный редактор всех модовых миров. Неподтверждённые скрипты, границы, collision/navmesh, рендеринг и safe arrival фиксировать отдельно. Игровую приёмку выполнять на общей итерации.

Фактически в этом шаге исследованы записи и указанные overrides; C++/ESP/CI не изменены, новые функции не заявляются.


## 6. Campaign40 — read-only каталог данных поместий

Команда: `python tools/build_guest_homes.py SNAPSHOT.zip loadorder.txt plugins.txt OUTPUT --estate-catalog`. Создаёт только CoopEstateEvidence.json (schema STM-ESTATE-EVIDENCE-1), без ESP/NAVM/WRLD, без установки на клиент или сервер. Встроены три стабильных vanilla anchors; winning QUST/VMAD, LCTN/CELL, REFR/ACHR, base records, XESP edges и reciprocal XTEL извлекаются автоматически. Строительные списки, другие quest links, прямые зависимости и пространственные данные разделены. Связанные удалённые объекты остаются диагностикой. Поля approved_boundary/approved_arrival = null, runtime_ready = false. Это вход для определения компактного разрешённого estate catalog, а не разрешение на применение объектов. Нет исполнения квестов/VM, runtime ACL, автоматической оценки collision/navmesh, окончательной границы, текущего строительного состояния или универсального определения сторонних estates.

Локально прогнан именно новый collector через отдельный harness с логикой существующего binary reader и stable key: исторические архив/порядок из §1, 95 активных/implicit plugins, 3 estates, 9749 reference rows, 658 base rows, 6 interior cells и 9 named exterior cells. Прямые construction refs Lakeview 651 (включая один Skyrim ref), Windstad 650, Heljarchen 650; missing/shared/foreign-interior в этих прямых списках 0. На каждый estate 15 направленных взаимных XTEL candidates: 13 выходов наружу, 2 направления одного basement pair. Это не проверка доступности двери или safe arrival. Синтетический check_estate_catalog.py прошёл локально (с точным stable helper); проверены VMAD, winning overrides, deletion/relocation, parent flags, XTEL size/flags, отказ overwrite, отсутствие разрешений. Полный repo CI ещё не подтверждён.

Исправлен сопутствующий дефект: все 85 XTEL в исходном HearthFires.esm имеют длину 32 bytes, последний uint32 flags = 0. Предыдущий классификатор L проверял 28 bytes, как и его синтетическая fixture. Теперь native capabilities принимает 32 bytes, flags 0, остальные консервативные ограничения сохранены; fixtures обновлены с отрицательными случаями. Старый генератор не меняется. Не утверждать, что все basement doors теперь проходят прежний allowlist (XLRL/другие поля и зависимости остаются). Не импортировать этот JSON как CoopNativeHomeObjects.txt.

Следующий шаг — compact estate context из этих данных, проверяемые vanilla spatial boundary/arrival и server admission/routing с сохранением обычного exterior grid. До реализации runtime исследовать реальные source paths Discovery/CellId/Character/Object/Inventory; не расширять interior private scope слепо. Отдельно сверить строгую L-классификацию на фактических дверях. Финальный effective profile, PEX/BSA, игровые тесты и рендеринг остаются открыты. Код C++ в данном коммите не меняется.

## Campaign46 — реальная транзитивная зависимость строительства

Проверка 25.11.2026, код 3f8e896: отдельный локальный read-only обход фактического исторического MO2 snapshot из §1 (95 plugins). Использованы MAST каждого winning QUST/REFR/ACHR, GRUP world/CELL, XESP и удаление/замещение при override; подсчитаны все достижимые дочерние ссылки от **261 прямой наружной REFR каждого BYOHHouseScript**, без отдельного добавления SiteEnableMarker, стола и улучшений. Все три winning QUST — исторический USSEP. Полная транзитивная группа не совпадает с C binding Campaign46.

| Поместье | Прямые наружные REFR | С ними и всеми потомками XESP | Дополнительные наружные | Дополнительные внутренние |
|---|---:|---:|---:|---:|
| Lakeview | 261 | 541 | 192 | 88 |
| Windstad | 261 | 588 | 242 | 85 |
| Heljarchen | 261 | 554 | 213 | 80 |

Пример HearthFires.esm:003145 (BYOHHouse1Room02BPart047Roof) в Tamriel persistent CELL с VMAD DLCHouseExteriorFinishRoomScript: четыре транзитивных потомка находятся в личной внутренней CELL 002FEC (003147, 00316C, 003170, 003174). Это структурная зависимость; код PEX, фактическое изменение квеста, коллизий или срабатывание события не проверены. Граф не означает, что каждая ссылка меняется при каждом вызове.

Отдельно SiteEnableMarker Lakeview 00307B: 39 прямых потомков (включая два ACHR 00326B/00326C, CraftingTrigger 0030B1 с BYOHHouseCraftingTriggerScript и три инверсных Skyrim.esm WETrigger 08F645/099C60/099C70). Всего 40 ссылок с самим корнем. Windstad 00A50E — 23 с корнем, Heljarchen 00C3C6 — 22. Fish hatchery root 0008D0 — 44 с корнем; дочерняя REFR 0008D3 содержит BYOHHouseFishHatcheryScript. Не переключать SiteEnableMarker ради доступа к чертёжному столу.

**Блокирующая развилка реализации:** простой перенос interior Prepare на несколько наружных CELL не покрывает эти связи. Текущий reversible overlay проверяет закрытые группы в одной CELL и отклоняет teleport/alias/неизвестные записи. Снять эти проверки или независимо форсировать потомков без проверки движка нельзя. Рекомендуется не создавать новый генератор мира: сначала проверить эффективные Hearthfire/USSEP PEX (в т.ч. DLCHouseExteriorFinishRoomScript, BYOHHouseCraftingTriggerScript, BYOHHouseFishHatcheryScript, BYOHHouseBuildingScript, BYOHHouseScript), затем ограниченный сквозной нативный сценарий на отдельном сохранении с исходным состоянием, изменением корня, readback потомков, восстановлением, контролем квестовых/имущественных побочных эффектов. В первую очередь решается одна строительная группа; не пытаться заранее покрыть все моды и все формы. Согласовать с пользователем переход к этому раннему игровому испытанию либо временное ограничение наружных визитов. До проверки не активировать exterior scope. Нет доказательства невозможности нативного подхода, но компиляция не подтверждает его безопасность. В этом шаге runtime не изменён.
