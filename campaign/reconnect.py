"""Durable per-character outbox for requests that were sent or being prepared when the link dropped."""
from dataclasses import asdict, dataclass
import hashlib
import json
import sqlite3

from replica import initialize_history
from world_journal import Rejected, Stage, Transfer, WorldAction


@dataclass(frozen=True)
class Cursor:
    revision: int
    digest: str


class HostHistory:
    """A proof of the committed event prefix lets a returned client detect a host rollback."""
    def __init__(self, journal):
        self.journal = journal
        self.lineage = initialize_history(journal)
        self.world = journal.db.execute("SELECT world_id, manifest FROM world").fetchone()

    def cursors(self, after, through=None):
        # The server adapter can maintain this chain incrementally at commit time.
        digest = hashlib.sha256(json.dumps((*self.world, self.lineage)).encode()).digest()
        history = []
        if after == 0:
            history.append(Cursor(0, digest.hex()))
        with self.journal._transaction(write=False):
            for revision, body in self.journal.db.execute("SELECT revision, body FROM events ORDER BY revision"):
                if through is not None and revision > through:
                    break
                digest = hashlib.sha256(digest + revision.to_bytes(8, "big") + body.encode()).digest()
                if revision >= after:
                    history.append(Cursor(revision, digest.hex()))
        return history

    def require_prefix(self, cursor):
        if self.cursors(cursor.revision, cursor.revision) != [cursor]:
            raise Rejected("different campaign history; keep both files for recovery")


class StaleRequest(Rejected):
    """Definitively rejected against the current authoritative state."""


class PeerLog:
    """Used independently on the host's game client and on the guest's game client."""
    def __init__(self, path, character, history):
        self.character = character
        self.db = sqlite3.connect(path)
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS peer(id INTEGER PRIMARY KEY CHECK(id=1), who TEXT, world TEXT,
                manifest TEXT, revision INTEGER, digest TEXT);
            CREATE TABLE IF NOT EXISTS outbox(seq INTEGER PRIMARY KEY, id TEXT UNIQUE, body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS rejected(id TEXT PRIMARY KEY, reason TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS interruptions(seq INTEGER PRIMARY KEY, reason TEXT, revision INTEGER, pending TEXT);
        """)
        genesis = history.cursors(0, 0)[0]
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO peer VALUES(1,?,?,?,?,?)",
                            (character, *history.world, genesis.revision, genesis.digest))
        if self.db.execute("SELECT who,world,manifest FROM peer").fetchone() != (character, *history.world):
            self.db.close()
            raise Rejected("wrong peer or campaign")

    def cursor(self):
        return Cursor(*self.db.execute("SELECT revision, digest FROM peer").fetchone())

    def queue(self, action):
        if action.actor != self.character or not action.action_id:
            raise Rejected("invalid local request")
        body = json.dumps(asdict(action), separators=(",", ":"), sort_keys=True)
        with self.db:
            rejected = self.db.execute("SELECT reason FROM rejected WHERE id=?", (action.action_id,)).fetchone()
            if rejected:
                raise StaleRequest(rejected[0])
            old = self.db.execute("SELECT body FROM outbox WHERE id=?", (action.action_id,)).fetchone()
            if old and old != (body,):
                raise Rejected("local request id reused")
            self.db.execute("INSERT OR IGNORE INTO outbox(id,body) VALUES(?,?)", (action.action_id, body))

    def interrupted(self, reason):
        with self.db:
            pending = [row[0] for row in self.db.execute("SELECT id FROM outbox ORDER BY seq")]
            self.db.execute("INSERT INTO interruptions(reason,revision,pending) VALUES(?,?,?)",
                            (reason, self.cursor().revision, json.dumps(pending)))

    def reconcile(self, host, commit_if_valid, apply_event):
        """Use only with a safe main-thread adapter. No game state changes while disconnected.

        commit_if_valid checks participant, ownership, expected version and game-specific
        prerequisites for an unconfirmed request. apply_event sets absolute target state,
        tolerating a repeated call after a crash before the cursor was written.
        """
        if tuple(host.world) != self.db.execute("SELECT world,manifest FROM peer").fetchone():
            raise Rejected("wrong campaign or manifest")
        host.require_prefix(self.cursor())
        resolved = []
        for req, body in self.db.execute("SELECT id,body FROM outbox ORDER BY seq").fetchall():
            raw = json.loads(body)
            action = WorldAction(req, self.character, tuple(Transfer(**v) for v in raw["transfers"]),
                                 Stage(**raw["stage"]) if raw["stage"] else None)
            found = host.journal.db.execute("SELECT body FROM requests WHERE action_id=?", (req,)).fetchone()
            if found:
                if json.loads(found[0]) != raw:
                    raise Rejected("host receipt conflicts with saved request")
            else:
                try:
                    commit_if_valid(action)
                except StaleRequest as exc:
                    with self.db:
                        self.db.execute("INSERT OR REPLACE INTO rejected VALUES(?,?)", (req, str(exc)))
                        self.db.execute("DELETE FROM outbox WHERE id=?", (req,))
                    continue
                receipt = host.journal.db.execute("SELECT body FROM requests WHERE action_id=?", (req,)).fetchone()
                if not receipt or json.loads(receipt[0]) != raw:
                    raise Rejected("request has no matching durable acknowledgement")
            resolved.append((req,))
        start = self.cursor().revision
        events = host.journal.events_after(start)
        if events:
            anchors = host.cursors(start + 1, events[-1].revision)
            if len(events) != len(anchors):
                raise Rejected("incomplete history")
            for event, anchor in zip(events, anchors):
                if event.revision != anchor.revision:
                    raise Rejected("event gap")
                apply_event(event)
                with self.db:
                    self.db.execute("UPDATE peer SET revision=?,digest=? WHERE id=1", (anchor.revision, anchor.digest))
        with self.db:
            self.db.executemany("DELETE FROM outbox WHERE id=?", resolved)
