"""Server-side contract for a shared conversation. IDs are stable forms/instances, never translated text.

Presentation and Skyrim scripts run on clients. The chosen reply, eligible attendees
and confirmed outcomes remain on the host. A choice never executes a Papyrus fragment
just because another character joined the conversation.
"""
from dataclasses import asdict, dataclass
import json

from world_journal import Rejected, Stage, Transfer, WorldAction


@dataclass(frozen=True)
class Effect:
    key: str
    # shared: once per choice; participant: each present at choice; speaker: personal action.
    scope: str
    transfers: tuple[Transfer, ...] = ()
    stage: Stage | None = None


def packed(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False, sort_keys=True)


class DialogueParty:
    def __init__(self, journal, party, members, creator):
        self.journal, self.db, self.party = journal, journal.db, party
        self.members = frozenset(members)
        if creator not in self.members or not party:
            raise Rejected("invalid party creator")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS dialogue_leaders(party TEXT PRIMARY KEY, person TEXT, term INTEGER);
            CREATE TABLE IF NOT EXISTS talks(id TEXT PRIMARY KEY, party TEXT, speaker TEXT, npc TEXT,
                closed INTEGER NOT NULL DEFAULT 0);
            CREATE UNIQUE INDEX IF NOT EXISTS one_open_speaker ON talks(party,speaker) WHERE closed=0;
            CREATE UNIQUE INDEX IF NOT EXISTS one_open_npc ON talks(party,npc) WHERE closed=0;
            CREATE TABLE IF NOT EXISTS listeners(talk TEXT, person TEXT, PRIMARY KEY(talk,person));
            CREATE TABLE IF NOT EXISTS choices(talk TEXT, step INTEGER, topic TEXT, attendees TEXT,
                effects TEXT, PRIMARY KEY(talk,step));
        """)
        self.db.execute("INSERT OR IGNORE INTO dialogue_leaders VALUES(?,?,1)", (party, creator))

    def leader(self):
        return self.db.execute("SELECT person,term FROM dialogue_leaders WHERE party=?", (self.party,)).fetchone()

    def handoff(self, sender, target, term):
        with self.journal._transaction():
            if self.leader() != (sender,term) or target not in self.members or sender == target:
                raise Rejected("stale or invalid leadership change")
            self.db.execute("UPDATE dialogue_leaders SET person=?, term=term+1 WHERE party=?", (target,self.party))

    def _talk(self, talk):
        row = self.db.execute("SELECT speaker,npc,closed FROM talks WHERE party=? AND id=?", (self.party,talk)).fetchone()
        if row is None:
            raise Rejected("unknown conversation")
        return row

    def start(self, talk, speaker, npc):
        if speaker not in self.members or not talk or not npc:
            raise Rejected("invalid speaker or target")
        with self.journal._transaction():
            existing = self.db.execute("SELECT party,speaker,npc,closed FROM talks WHERE id=?", (talk,)).fetchone()
            if existing:
                if existing[:3] != (self.party,speaker,npc):
                    raise Rejected("session identity conflicts")
                return
            if self.db.execute("SELECT 1 FROM talks WHERE party=? AND closed=0 AND (speaker=? OR npc=?)",
                               (self.party,speaker,npc)).fetchone():
                raise Rejected("conversation is already in progress")
            self.db.execute("INSERT INTO talks(id,party,speaker,npc) VALUES(?,?,?,?)", (talk,self.party,speaker,npc))
            self.db.execute("DELETE FROM listeners WHERE person=? AND talk IN (SELECT id FROM talks WHERE party=?)",
                            (speaker,self.party))

    def observe(self, talk, character, present):
        with self.journal._transaction():
            speaker, _, closed = self._talk(talk)
            if character not in self.members or character == speaker or closed:
                raise Rejected("unavailable listener")
            if present:
                if self.db.execute("SELECT 1 FROM talks WHERE speaker=? AND party=? AND closed=0",
                                   (character,self.party)).fetchone():
                    raise Rejected("listener is speaking elsewhere")
                self.db.execute("DELETE FROM listeners WHERE person=? AND talk IN (SELECT id FROM talks WHERE party=?)",
                                (character,self.party))
                self.db.execute("INSERT INTO listeners VALUES(?,?)", (talk,character))
            else:
                self.db.execute("DELETE FROM listeners WHERE talk=? AND person=?", (talk,character))

    def choose(self, talk, speaker, step, topic):
        """The participant list is frozen at this reply, never reconstructed from current distance."""
        with self.journal._transaction():
            owner, _, closed = self._talk(talk)
            if owner != speaker or not topic:
                raise Rejected("only the speaker chooses")
            existing = self.db.execute("SELECT topic,attendees FROM choices WHERE talk=? AND step=?",
                                       (talk,step)).fetchone()
            if existing:
                if existing[0] != topic:
                    raise Rejected("different reply for the same turn")
                return tuple(json.loads(existing[1]))
            latest = self.db.execute("SELECT COALESCE(MAX(step),0) FROM choices WHERE talk=?", (talk,)).fetchone()[0]
            if closed or type(step) is not int or step != latest + 1:
                raise Rejected("closed or out-of-order choice")
            attendees = [speaker] + [row[0] for row in self.db.execute(
                "SELECT person FROM listeners WHERE talk=? ORDER BY person", (talk,))]
            self.db.execute("INSERT INTO choices VALUES(?,?,?,?,NULL)", (talk,step,topic,packed(attendees)))
            return tuple(attendees)

    def confirm_outcome(self, talk, step, leader, term, effects):
        """Record classified effects once, even if the speaker is the guest or the menu already closed.

        $recipient in a transfer holder is substituted only by the host's game adapter here.
        A personally scoped quest run also includes the recipient identity.
        """
        effects = tuple(effects)
        spec = packed([asdict(e) for e in effects])
        with self.journal._transaction():
            speaker, _, _ = self._talk(talk)
            row = self.db.execute("SELECT attendees,effects FROM choices WHERE talk=? AND step=?", (talk,step)).fetchone()
            if not row:
                raise Rejected("no such choice")
            if row[1] is not None:
                if row[1] != spec:
                    raise Rejected("outcome differs from its receipt")
                return False
            if self.leader() != (leader,term):
                raise Rejected("outcome must be confirmed by the current leader")
            keys = set()
            for effect in effects:
                if not effect.key or effect.key in keys or effect.scope not in ("shared","participant","speaker"):
                    raise Rejected("ambiguous effect identity or scope")
                keys.add(effect.key)
                recipients = json.loads(row[0]) if effect.scope == "participant" else [speaker if effect.scope == "speaker" else None]
                for recipient in recipients:
                    def holder(value):
                        if value == "$recipient":
                            if recipient is None:
                                raise Rejected("a shared transfer must name its real endpoints")
                            return recipient
                        return value
                    moves = tuple(Transfer(holder(v.source),holder(v.destination),v.item,v.quantity) for v in effect.transfers)
                    stage = effect.stage
                    if stage and recipient is not None:
                        stage = Stage(stage.quest, packed([stage.run,recipient]), stage.stage)
                    op = packed(["dialogue", self.party, talk, step, effect.key, recipient])
                    self.journal._commit_locked(WorldAction(op,speaker,moves,stage))
            self.db.execute("UPDATE choices SET effects=? WHERE talk=? AND step=?", (spec,talk,step))
            return True

    def close(self, talk, speaker):
        with self.journal._transaction():
            if self._talk(talk)[0] != speaker:
                raise Rejected("only the speaker closes the conversation")
            self.db.execute("UPDATE talks SET closed=1 WHERE id=?", (talk,))
            self.db.execute("DELETE FROM listeners WHERE talk=?", (talk,))
