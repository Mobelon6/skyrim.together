"""Reference adapter for a campaign journal and independently saved co-op characters.

The engine adapter must bracket an actual game save, registering the digest only
after the save and its co-save are complete. Replaying a plan sets absolute stack
counts under the game's network-echo guard; transient UI state is not replayed.
"""
from dataclasses import dataclass
import re

from world_journal import Rejected, Stack, WorldJournal

DIGEST = re.compile(r"[0-9a-f]{64}")


@dataclass(frozen=True)
class RecoveryPlan:
    from_revision: int
    through_revision: int
    stacks: tuple[Stack, ...]


class SaveRecovery:
    def __init__(self, journal: WorldJournal):
        self.journal = journal
        self.db = journal.db
        self.db.execute("""
            CREATE TABLE IF NOT EXISTS character_saves (
                digest TEXT PRIMARY KEY,
                holder TEXT NOT NULL,
                revision INTEGER NOT NULL CHECK (revision >= 0)
            )
        """)

    def _latest_revision(self):
        return self.db.execute("SELECT COALESCE(MAX(revision), 0) FROM events").fetchone()[0]

    def register_completed_save(self, holder: str, digest: str, applied_revision: int):
        """applied_revision is captured at the save boundary, never inferred from the current server head."""
        if not holder or not DIGEST.fullmatch(digest):
            raise Rejected("invalid save identity")
        with self.journal._transaction():
            if type(applied_revision) is not int or not 0 <= applied_revision <= self._latest_revision():
                raise Rejected("save revision is outside campaign history")
            old = self.db.execute("SELECT holder, revision FROM character_saves WHERE digest=?", (digest,)).fetchone()
            if old is None:
                self.db.execute("INSERT INTO character_saves VALUES (?, ?, ?)", (digest, holder, applied_revision))
            elif old != (holder, applied_revision):
                raise Rejected("save already bound to a different checkpoint")

    def plan_for_save(self, holder: str, digest: str) -> RecoveryPlan:
        """Reconstruct journal-managed inventory changes for one recognized save.

        World, house, quest and NPC records require their own game adapters. This
        method deliberately omits them. The database is bound to a world and mod
        manifest when the caller opens WorldJournal.
        """
        with self.journal._transaction(write=False):
            saved = self.db.execute("SELECT revision FROM character_saves WHERE digest=? AND holder=?",
                                    (digest, holder)).fetchone()
            if not saved:
                raise Rejected("save does not belong to this character and campaign")
            end = self._latest_revision()
            if saved[0] > end:
                raise Rejected("save is ahead of the available journal")
            targets = {}
            for event in self.journal.events_after(saved[0]):
                for stack in event.stacks:
                    if stack.holder == holder:
                        targets[stack.item] = stack
            return RecoveryPlan(saved[0], end, tuple(targets[item] for item in sorted(targets)))
