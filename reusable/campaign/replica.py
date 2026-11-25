"""Portable replica of a committed campaign database. Single host publisher.

The transport provides a binary stream. Downloads are verified in the destination
directory before installation. A guest never writes back from its snapshot on
reconnect. The local catalog keeps the last two completed checkpoints.
"""
from dataclasses import dataclass
import hashlib
from pathlib import Path
import sqlite3
import tempfile
import uuid
import os


BLOCK = 1024 * 1024


class ReplicaError(ValueError):
    pass


@dataclass(frozen=True)
class Checkpoint:
    campaign: str
    manifest: str
    lineage: str
    sequence: int
    ticket: str
    revision: int
    size: int
    sha256: str


def _inspect(path, parent=None):
    uri = Path(path).resolve().as_uri() + "?mode=ro&immutable=1"
    db = sqlite3.connect(uri, uri=True)
    try:
        if db.execute("PRAGMA quick_check").fetchone() != ("ok",):
            raise ReplicaError("invalid checkpoint database")
        identity = db.execute("SELECT world_id, manifest FROM world WHERE singleton=1").fetchone()
        generation = db.execute("SELECT lineage, sequence FROM replica_generation WHERE id=1").fetchone()
        if not identity or not generation:
            raise ReplicaError("incomplete campaign identity")
        if parent and db.execute("SELECT ticket FROM replica_history WHERE sequence=?", (parent[0],)).fetchone() != (parent[1],):
            raise ReplicaError("checkpoint branches before the installed copy")
        ticket = db.execute("SELECT ticket FROM replica_history WHERE sequence=?", (generation[1],)).fetchone()
        if not ticket:
            raise ReplicaError("missing checkpoint history")
        revision = db.execute("SELECT COALESCE(MAX(revision), 0) FROM events").fetchone()[0]
        return (*identity, *generation, *ticket, revision)
    finally:
        db.close()


def _digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(BLOCK):
            h.update(block)
    return h.hexdigest()


def initialize_history(journal):
    """Create a persistent lineage before the first peer connects or snapshot is taken."""
    with journal._transaction():
        journal.db.execute("""CREATE TABLE IF NOT EXISTS replica_generation(
            id INTEGER PRIMARY KEY CHECK(id=1), lineage TEXT NOT NULL, sequence INTEGER NOT NULL)""")
        journal.db.execute("CREATE TABLE IF NOT EXISTS replica_history(sequence INTEGER PRIMARY KEY, ticket TEXT NOT NULL)")
        journal.db.execute("INSERT OR IGNORE INTO replica_generation VALUES(1, ?, 0)", (uuid.uuid4().hex,))
        return journal.db.execute("SELECT lineage FROM replica_generation WHERE id=1").fetchone()[0]


class HostPublisher:
    def __init__(self, journal, interval=60):
        if interval <= 0:
            raise ValueError("interval must be positive")
        self.journal = journal
        self.interval = interval
        self.next_due = None
        initialize_history(journal)

    def publish_if_due(self, now, send, *, force=False):
        """send(checkpoint, reader) returns after the guest durably acknowledges it."""
        if not force and self.next_due is not None and now < self.next_due:
            return False
        with self.journal._transaction():
            self.journal.db.execute("UPDATE replica_generation SET sequence=sequence+1 WHERE id=1")
            self.journal.db.execute("INSERT INTO replica_history SELECT sequence, ? FROM replica_generation WHERE id=1",
                                    (uuid.uuid4().hex,))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "snapshot.sqlite")
            copy = sqlite3.connect(path)
            try:
                self.journal.db.backup(copy, pages=256)
                # Make the exported file independently readable without its WAL.
                if copy.execute("PRAGMA journal_mode=DELETE").fetchone()[0] != "delete":
                    raise ReplicaError("checkpoint could not be sealed")
            finally:
                copy.close()
            checkpoint = Checkpoint(*_inspect(path), path.stat().st_size, _digest(path))
            with path.open("rb") as stream:
                send(checkpoint, stream)
        self.next_due = now + self.interval
        return True


class GuestReplica:
    def __init__(self, directory, campaign, manifest):
        self.root = Path(directory)
        self.root.mkdir(parents=True, exist_ok=True)
        self.campaign, self.manifest = campaign, manifest
        self.db = sqlite3.connect(self.root / "index.sqlite", isolation_level=None)
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("""CREATE TABLE IF NOT EXISTS copies(
            sequence INTEGER PRIMARY KEY, lineage TEXT NOT NULL, revision INTEGER NOT NULL,
            digest TEXT NOT NULL, filename TEXT NOT NULL, ticket TEXT NOT NULL)""")
        self.db.execute("CREATE TABLE IF NOT EXISTS identity(id INTEGER PRIMARY KEY CHECK(id=1), campaign TEXT, manifest TEXT)")
        self.db.execute("INSERT OR IGNORE INTO identity VALUES(1,?,?)", (campaign, manifest))
        if self.db.execute("SELECT campaign, manifest FROM identity").fetchone() != (campaign, manifest):
            self.db.close()
            raise ReplicaError("choose the directory of this campaign")

    def _latest(self):
        return self.db.execute("SELECT sequence, lineage, revision, digest, filename, ticket FROM copies ORDER BY sequence DESC LIMIT 1").fetchone()

    def current(self):
        row = self._latest()
        return self.root / row[4] if row else None

    def receive(self, checkpoint, reader):
        if (checkpoint.campaign, checkpoint.manifest) != (self.campaign, self.manifest):
            raise ReplicaError("checkpoint belongs to another campaign")
        self.db.execute("BEGIN IMMEDIATE")
        tmp = None
        stale = []
        try:
            last = self._latest()
            if last is not None:
                if checkpoint.lineage != last[1] or checkpoint.sequence < last[0] or checkpoint.revision < last[2]:
                    raise ReplicaError("older or different campaign history")
                if checkpoint.sequence == last[0]:
                    if checkpoint.sha256 != last[3]:
                        raise ReplicaError("conflicting checkpoint")
                    if self.current().is_file() and _digest(self.current()) == last[3]:
                        self.db.commit()
                        return
            digest = hashlib.sha256()
            with tempfile.NamedTemporaryFile(dir=self.root, suffix=".partial", delete=False) as out:
                tmp = Path(out.name)
                written = 0
                while block := reader.read(BLOCK):
                    written += len(block)
                    if written > checkpoint.size:
                        raise ReplicaError("checkpoint exceeds its advertised size")
                    out.write(block)
                    digest.update(block)
                out.flush()
                os.fsync(out.fileno())
            if written != checkpoint.size or digest.hexdigest() != checkpoint.sha256:
                raise ReplicaError("incomplete checkpoint")
            expected = (checkpoint.campaign, checkpoint.manifest, checkpoint.lineage, checkpoint.sequence, checkpoint.ticket, checkpoint.revision)
            if _inspect(tmp, (last[0], last[5]) if last else None) != expected:
                raise ReplicaError("checkpoint metadata differs from its contents")
            name = f"campaign-{checkpoint.sequence}-{checkpoint.sha256}.sqlite"
            os.replace(tmp, self.root / name)
            if os.name == "posix":
                fd = os.open(self.root, os.O_RDONLY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
            self.db.execute("INSERT OR REPLACE INTO copies VALUES (?,?,?,?,?,?)",
                            (checkpoint.sequence, checkpoint.lineage, checkpoint.revision, checkpoint.sha256, name, checkpoint.ticket))
            stale = self.db.execute("SELECT sequence, filename FROM copies ORDER BY sequence DESC LIMIT -1 OFFSET 2").fetchall()
            self.db.executemany("DELETE FROM copies WHERE sequence=?", [(seq,) for seq, _ in stale])
            self.db.commit()
        except BaseException:
            self.db.rollback()
            raise
        finally:
            if tmp is not None:
                tmp.unlink(missing_ok=True)
        for _, filename in stale:
            (self.root / filename).unlink(missing_ok=True)
