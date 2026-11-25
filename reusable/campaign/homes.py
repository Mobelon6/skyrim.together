"""Reference storage for individually owned copies of the same bought house."""
import json

from world_journal import Rejected, Transfer, WorldAction


class PlayerHomes:
    def __init__(self, journal):
        self.journal = journal
        self.db = journal.db
        self.db.execute("""CREATE TABLE IF NOT EXISTS houses(
            template TEXT NOT NULL, owner TEXT NOT NULL, PRIMARY KEY(template, owner))""")

    def record_purchase(self, template, owner):
        """Called only after the game adapter confirms this character's purchase."""
        if not template or not owner:
            raise Rejected("missing house or buyer identity")
        with self.journal._transaction():
            self.db.execute("INSERT OR IGNORE INTO houses VALUES(?, ?)", (template, owner))

    def choices(self, template, present_party):
        owners = self.db.execute("SELECT owner FROM houses WHERE template=? ORDER BY owner", (template,))
        return tuple(owner for (owner,) in owners if owner in present_party)

    def _container(self, template, owner, ref):
        if not ref or not self.db.execute("SELECT 1 FROM houses WHERE template=? AND owner=?",
                                          (template, owner)).fetchone():
            raise Rejected("unknown house or container")
        return json.dumps(("home", template, owner, ref), separators=(",", ":"))

    def contents(self, template, owner, ref):
        """The caller routes this read to a visitor admitted to that instance."""
        with self.journal._transaction(write=False):
            container = self._container(template, owner, ref)
            return tuple(self.db.execute("SELECT item,quantity FROM stacks WHERE holder=? ORDER BY item", (container,)))

    def move(self, request, visitor, template, owner, ref, item, quantity, *, take):
        if visitor != owner or not request:
            raise Rejected("only the house owner may change this container")
        with self.journal._transaction():
            container = self._container(template, owner, ref)
            source, target = (container, owner) if take else (owner, container)
            operation = json.dumps(("home-move", owner, request), separators=(",", ":"))
            return self.journal._commit_locked(WorldAction(operation, owner, (Transfer(source, target, item, quantity),)))
