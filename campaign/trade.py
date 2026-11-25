"""Durable two-player trade for the reference campaign. All inventory mutations use WorldJournal.

Game adapters supply stable character/stack identities and validate party membership,
range, menu availability and item transferability before requesting a new interaction.
"""
from dataclasses import dataclass
import json

from world_journal import Rejected, Transfer, WorldAction, WorldJournal


@dataclass(frozen=True)
class Offer:
    depositor: str
    item: str
    quantity: int


def _key(*parts):
    return json.dumps(parts, ensure_ascii=False, separators=(",", ":"))


class PlayerTrades:
    def __init__(self, journal: WorldJournal):
        self.journal = journal
        self.db = journal.db
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS trades(
                session TEXT PRIMARY KEY, inviter TEXT NOT NULL, guest TEXT NOT NULL,
                state TEXT NOT NULL CHECK(state IN ('invited','open','closed')),
                CHECK(inviter <> guest)
            );
            CREATE TABLE IF NOT EXISTS trade_seats(
                character TEXT PRIMARY KEY, session TEXT NOT NULL REFERENCES trades(session)
            );
        """)

    def invite(self, session, inviter, guest):
        if not session or not inviter or not guest or inviter == guest:
            raise Rejected("invalid invitation")
        with self.journal._transaction():
            existing = self.db.execute("SELECT inviter, guest, state FROM trades WHERE session=?", (session,)).fetchone()
            if existing:
                if existing[:2] != (inviter, guest):
                    raise Rejected("session id reused for different players")
                return existing[2]
            if self.db.execute("SELECT 1 FROM trade_seats WHERE character IN (?, ?)", (inviter, guest)).fetchone():
                raise Rejected("player is already in a trade")
            self.db.execute("INSERT INTO trades VALUES (?, ?, ?, 'invited')", (session, inviter, guest))
            self.db.executemany("INSERT INTO trade_seats VALUES (?, ?)", [(inviter, session), (guest, session)])
            return "invited"

    def _participants(self, session, character):
        row = self.db.execute("SELECT inviter, guest, state FROM trades WHERE session=?", (session,)).fetchone()
        if not row or character not in row[:2]:
            raise Rejected("unknown trade or character")
        return row

    def accept(self, session, character):
        with self.journal._transaction():
            _, guest, state = self._participants(session, character)
            if character != guest or state == "closed":
                raise Rejected("invitation is not available to this character")
            self.db.execute("UPDATE trades SET state='open' WHERE session=?", (session,))

    def _offers(self, session, participants):
        offers = []
        for depositor in participants:
            holder = _key("trade-buffer", session, depositor)
            rows = self.db.execute("SELECT item, quantity FROM stacks WHERE holder=? ORDER BY item", (holder,))
            offers.extend(Offer(depositor, item, quantity) for item, quantity in rows)
        return tuple(offers)

    def offers(self, session, character):
        with self.journal._transaction(write=False):
            inviter, guest, state = self._participants(session, character)
            if state != "open":
                raise Rejected("trade is not open")
            return self._offers(session, (inviter, guest))

    def move(self, session, request, character, depositor, item, quantity, *, take):
        if not request:
            raise Rejected("operation id is required")
        with self.journal._transaction():
            inviter, guest, state = self._participants(session, character)
            if depositor not in (inviter, guest) or (not take and depositor != character):
                raise Rejected("invalid depositor")
            buffer = _key("trade-buffer", session, depositor)
            source, destination = (buffer, character) if take else (character, buffer)
            operation = _key("trade-change", session, character, request)
            if state != "open" and not self.db.execute("SELECT 1 FROM requests WHERE action_id=?", (operation,)).fetchone():
                raise Rejected("trade is not open")
            transfer = Transfer(source, destination, item, quantity)
            return self.journal._commit_locked(WorldAction(operation, character, (transfer,)))

    def close(self, session, character):
        """Decline, cancel, disconnect or expire. Return unclaimed lots to their depositors."""
        with self.journal._transaction():
            inviter, guest, state = self._participants(session, character)
            if state == "closed":
                return
            refunds = tuple(
                Transfer(_key("trade-buffer", session, offer.depositor), offer.depositor, offer.item, offer.quantity)
                for offer in self._offers(session, (inviter, guest))
            )
            if refunds:
                self.journal._commit_locked(WorldAction(_key("trade-close", session), character, refunds))
            self.db.execute("UPDATE trades SET state='closed' WHERE session=?", (session,))
            self.db.execute("DELETE FROM trade_seats WHERE session=?", (session,))

    def recover_on_server_start(self):
        """Call once before admitting connections to a restarted server.

        Each refund and closure commits together. A second restart midway through
        cleanup only sees sessions that still need work.
        """
        pending = self.db.execute(
            "SELECT session, inviter FROM trades WHERE state <> 'closed' ORDER BY session"
        ).fetchall()
        for session, inviter in pending:
            self.close(session, inviter)
