"""Executable reference for committed co-op world events on one local SQLite file.

An upstream adapter must authorize and normalize game actions before commit.
This module owns the durable state and makes no calls into the game.
"""

from contextlib import contextmanager
from dataclasses import asdict, dataclass
import json
import sqlite3

MAX_STACK = (1 << 31) - 1

SCHEMA = """
CREATE TABLE IF NOT EXISTS world (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    world_id TEXT NOT NULL,
    manifest TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS stacks (
    holder TEXT NOT NULL,
    item TEXT NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity BETWEEN 1 AND 2147483647),
    PRIMARY KEY (holder, item)
);
CREATE TABLE IF NOT EXISTS events (
    revision INTEGER PRIMARY KEY,
    body TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS requests (
    action_id TEXT PRIMARY KEY,
    body TEXT NOT NULL,
    revision INTEGER NOT NULL REFERENCES events(revision)
);
CREATE TABLE IF NOT EXISTS stages (
    quest TEXT NOT NULL,
    run TEXT NOT NULL,
    stage INTEGER NOT NULL CHECK (stage BETWEEN 0 AND 65535),
    revision INTEGER NOT NULL REFERENCES events(revision),
    PRIMARY KEY (quest, run, stage)
);
"""


class Rejected(ValueError):
    """An event cannot be committed to the current model."""


@dataclass(frozen=True)
class Stack:
    holder: str
    item: str
    quantity: int


@dataclass(frozen=True)
class Transfer:
    source: str
    destination: str
    item: str
    quantity: int


@dataclass(frozen=True)
class Stage:
    quest: str
    run: str
    stage: int


@dataclass(frozen=True)
class WorldAction:
    action_id: str
    actor: str
    transfers: tuple[Transfer, ...] = ()
    stage: Stage | None = None


@dataclass(frozen=True)
class Event:
    revision: int
    actor: str
    stage: Stage | None
    stacks: tuple[Stack, ...]


@dataclass(frozen=True)
class CommitResult:
    event: Event
    applied: bool


@dataclass(frozen=True)
class Snapshot:
    revision: int
    stacks: tuple[Stack, ...]
    stages: tuple[Stage, ...]


class WorldJournal:
    def __init__(self, path, world_id: str, manifest: str,
                 initial_stacks: tuple[Stack, ...] = ()):
        if not world_id or not manifest:
            raise ValueError("world and manifest are required")
        self.db = sqlite3.connect(path, isolation_level=None, timeout=5)
        try:
            self.db.execute("PRAGMA foreign_keys = ON")
            mode = self.db.execute("PRAGMA journal_mode = WAL").fetchone()[0]
            if mode != "wal":
                raise RuntimeError("journal requires a local WAL database")
            self.db.execute("PRAGMA synchronous = FULL")
            self.db.executescript(SCHEMA)
            with self._transaction():
                existing = self.db.execute(
                    "SELECT world_id, manifest FROM world WHERE singleton = 1"
                ).fetchone()
                if existing is None:
                    self.db.execute("INSERT INTO world VALUES (1, ?, ?)", (world_id, manifest))
                    for stack in initial_stacks:
                        if not stack.holder or not stack.item:
                            raise ValueError("stack identities are required")
                        _check_quantity(stack.quantity)
                        self.db.execute(
                            "INSERT INTO stacks VALUES (?, ?, ?)",
                            (stack.holder, stack.item, stack.quantity),
                        )
                elif existing != (world_id, manifest):
                    raise Rejected("world or content manifest mismatch")
                elif initial_stacks:
                    raise Rejected("existing worlds cannot be initialized again")
        except BaseException:
            self.db.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.db.close()

    @contextmanager
    def _transaction(self, write: bool = True):
        self.db.execute("BEGIN IMMEDIATE" if write else "BEGIN")
        try:
            yield
            self.db.commit()
        except BaseException:
            self.db.rollback()
            raise

    def commit(self, action: WorldAction) -> CommitResult:
        """Commit a host-validated action, returning its event after SQLite commits."""
        with self._transaction():
            return self._commit_locked(action)

    def _commit_locked(self, action: WorldAction) -> CommitResult:
        """Caller holds this journal's write transaction."""
        _check_action(action)
        request = json.dumps(asdict(action), sort_keys=True, separators=(",", ":"))

        previous = self.db.execute(
            "SELECT body, revision FROM requests WHERE action_id = ?",
            (action.action_id,),
        ).fetchone()
        if previous is not None:
            if previous[0] != request:
                raise Rejected("action ID already belongs to another request")
            return CommitResult(self._event(previous[1]), False)

        stage = action.stage
        existing_stage = None
        if stage is not None:
            existing_stage = self.db.execute(
                "SELECT revision FROM stages WHERE quest = ? AND run = ? AND stage = ?",
                (stage.quest, stage.run, stage.stage),
            ).fetchone()

        if existing_stage is not None:
            self._remember_request(action.action_id, request, existing_stage[0])
            return CommitResult(self._event(existing_stage[0]), False)

        touched: dict[tuple[str, str], Stack] = {}
        for change in action.transfers:
            for stack in self._transfer(change):
                touched[(stack.holder, stack.item)] = stack
        final_stacks = tuple(touched.values())
        event_body = {
            "actor": action.actor,
            "stage": asdict(stage) if stage is not None else None,
            "stacks": [asdict(stack) for stack in final_stacks],
        }
        revision = self.db.execute(
            "INSERT INTO events (body) VALUES (?)", (json.dumps(event_body),)
        ).lastrowid
        if stage is not None:
            self.db.execute(
                "INSERT INTO stages VALUES (?, ?, ?, ?)",
                (stage.quest, stage.run, stage.stage, revision),
            )
        self._remember_request(action.action_id, request, revision)
        return CommitResult(Event(revision, action.actor, stage, final_stacks), True)

    def _remember_request(self, action_id: str, body: str, revision: int):
        self.db.execute("INSERT INTO requests VALUES (?, ?, ?)", (action_id, body, revision))

    def _quantity(self, holder: str, item: str) -> int:
        row = self.db.execute(
            "SELECT quantity FROM stacks WHERE holder = ? AND item = ?", (holder, item)
        ).fetchone()
        return row[0] if row is not None else 0

    def _set_stack(self, stack: Stack):
        key = (stack.holder, stack.item)
        if stack.quantity == 0:
            self.db.execute("DELETE FROM stacks WHERE holder = ? AND item = ?", key)
        else:
            self.db.execute(
                """INSERT INTO stacks VALUES (?, ?, ?)
                   ON CONFLICT(holder, item) DO UPDATE SET quantity = excluded.quantity""",
                (*key, stack.quantity),
            )

    def _transfer(self, change: Transfer) -> tuple[Stack, Stack]:
        source = self._quantity(change.source, change.item)
        destination = self._quantity(change.destination, change.item)
        if source < change.quantity:
            raise Rejected("insufficient source quantity")
        if destination + change.quantity > MAX_STACK:
            raise Rejected("destination stack is full")
        debit = Stack(change.source, change.item, source - change.quantity)
        credit = Stack(change.destination, change.item, destination + change.quantity)
        self._set_stack(debit)
        self._set_stack(credit)
        return debit, credit

    def _event(self, revision: int) -> Event:
        body = self.db.execute("SELECT body FROM events WHERE revision = ?", (revision,)).fetchone()[0]
        return _decode_event(revision, body)

    def events_after(self, revision: int) -> tuple[Event, ...]:
        return tuple(
            _decode_event(seq, body) for seq, body in self.db.execute(
                "SELECT revision, body FROM events WHERE revision > ? ORDER BY revision", (revision,)
            )
        )

    def snapshot(self) -> Snapshot:
        with self._transaction(write=False):
            revision = self.db.execute("SELECT COALESCE(MAX(revision), 0) FROM events").fetchone()[0]
            stacks = tuple(Stack(*row) for row in self.db.execute(
                "SELECT holder, item, quantity FROM stacks ORDER BY holder, item"
            ))
            stages = tuple(Stage(*row) for row in self.db.execute(
                "SELECT quest, run, stage FROM stages ORDER BY revision"
            ))
            return Snapshot(revision, stacks, stages)


def _decode_event(revision: int, body: str) -> Event:
    decoded = json.loads(body)
    stage = Stage(**decoded["stage"]) if decoded["stage"] is not None else None
    stacks = tuple(Stack(**stack) for stack in decoded["stacks"])
    return Event(revision, decoded["actor"], stage, stacks)


def _check_quantity(quantity: int):
    if type(quantity) is not int or not 1 <= quantity <= MAX_STACK:
        raise Rejected("invalid quantity")


def _check_action(action: WorldAction):
    if not action.action_id or not action.actor or (not action.transfers and action.stage is None):
        raise Rejected("action, actor and effects are required")
    for transfer in action.transfers:
        if not transfer.source or not transfer.destination or not transfer.item:
            raise Rejected("transfer identities are required")
        if transfer.source == transfer.destination:
            raise Rejected("transfer requires two distinct holders")
        _check_quantity(transfer.quantity)
    stage = action.stage
    if stage is not None and (
        not stage.quest or not stage.run or type(stage.stage) is not int
        or not 0 <= stage.stage <= 65535
    ):
        raise Rejected("invalid stage identity")
