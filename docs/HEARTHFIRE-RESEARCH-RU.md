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
