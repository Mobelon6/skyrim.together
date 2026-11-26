# STM — Skyrim Together для Skyrim SE 1.6.1170

Исполняемый код STM находится в `src/`. Эти файлы заменяют одноимённые файлы закреплённой версии TiltedEvolution и двух её библиотек перед компиляцией. Правки игрового поведения вносите непосредственно в исходники. Каталоги `patches/` сохранены для истории и разбора зависимостей. Обе сборки используют `src/`.

Рабочая ветка: `development/coop-dialogue-recovery`. Цель — полноценное совместное прохождение Skyrim в одном согласованном мире, без сетевых блокировок прогресса, потерь и дублирования.

Начало работы: [AGENTS.md](AGENTS.md). Актуальные задачи и следующий шаг: [COOP-STATUS.md](docs/COOP-STATUS.md). Журнал: [WORKLOG.md](docs/WORKLOG.md).

## Структура

| Каталог | Назначение |
|---|---|
| [required/](required/) | Нужное: утверждённая основа |
| [important/](important/) | Важное: утверждённый, незавершённый функционал |
| [reusable/](reusable/) | Лишнее сейчас: полезные наработки и модели |
| [obsolete/](obsolete/) | Лишнее: отменённые механизмы; часть ещё нужна как зависимость |
| [docs/](docs/) | Требования, состояние, журнал, аудит и история |
| [.github/workflows/](.github/workflows/) | Порядок применения патчей, сборка, проверки |

В каждой категории код находится в patches/, проверки в tests/, утилиты в tools/ (если имеются). Полная [карта структуры](docs/REPOSITORY-STRUCTURE.md) содержит назначение и правила работы с зависимостями. Не считать перенос в obsolete отключением runtime.

[Методика и сборка](DEVELOPMENT.md) · [Спецификация кампании](docs/Coop-Campaign-RU.md) · [Недвижимость](docs/HOME-CONCEPT-RU.md) · [Диалоги](docs/Party-Dialogue-and-Recovery-RU.md) · [Аудит](docs/COOP-AUDIT-2026-11-25.md) · [Лицензии](LICENSING.md) · [Actions](https://github.com/Mobelon6/skyrim.together/actions)

Доступность установочного архива и результаты игры проверять по COOP-STATUS. История предыдущих версий сохранена в [COOP-HISTORY.md](docs/COOP-HISTORY.md) и [прежнем README](https://github.com/Mobelon6/skyrim.together/blob/ea01827d081e8c0e68dd7151a1a106622c34de22/README.md).
