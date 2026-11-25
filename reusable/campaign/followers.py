"""Reference rules for the vanilla humanoid follower slot in one campaign database.

The client evaluates the subject's actual dialogue conditions. A positive eligibility
check applies to this request only. It can change with relationship, quest or faction.
"""
from dataclasses import dataclass

from world_journal import Rejected, WorldJournal


@dataclass(frozen=True)
class FollowerReply:
    option: str
    generation: int


class FollowerSlots:
    def __init__(self, journal: WorldJournal):
        self.journal = journal
        self.db = journal.db
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS follower_slots (
                npc TEXT PRIMARY KEY, employer TEXT UNIQUE, generation INTEGER NOT NULL
            );
        """)

    def _role(self, npc, character, eligible):
        row = self.db.execute("SELECT employer, generation FROM follower_slots WHERE npc=?", (npc,)).fetchone()
        employer, generation = row or (None, 0)
        if employer == character:
            return FollowerReply("companion_commands", generation)
        if not eligible:
            return FollowerReply("personal_prerequisites", generation)
        has_follower = self.db.execute("SELECT 1 FROM follower_slots WHERE employer=?", (character,)).fetchone()
        if employer is not None or has_follower:
            return FollowerReply("follow_me_then_refuse", generation)
        return FollowerReply("follow_me", generation)

    def dialogue(self, npc, character, *, eligible):
        if not npc or not character:
            raise Rejected("follower and character identities are required")
        with self.journal._transaction(write=False):
            return self._role(npc, character, eligible)

    def recruit(self, npc, character, generation, *, eligible):
        if not npc or not character:
            raise Rejected("follower and character identities are required")
        with self.journal._transaction():
            reply = self._role(npc, character, eligible)
            if reply.option != "follow_me" or reply.generation != generation:
                return reply
            self.db.execute("""INSERT INTO follower_slots(npc, employer, generation) VALUES (?, ?, 1)
                ON CONFLICT(npc) DO UPDATE SET employer=excluded.employer, generation=follower_slots.generation+1""",
                            (npc, character))
            return FollowerReply("companion_commands", generation + 1)

    def dismiss(self, npc, character, generation):
        with self.journal._transaction():
            changed = self.db.execute("""UPDATE follower_slots SET employer=NULL, generation=generation+1
                WHERE npc=? AND employer=? AND generation=?""", (npc, character, generation)).rowcount
            return changed == 1
