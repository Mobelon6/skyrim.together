import hashlib
from pathlib import Path
import tempfile
import unittest

from trade import PlayerTrades
from save_recovery import RecoveryPlan, SaveRecovery
from world_journal import Rejected, Stack, Transfer, WorldAction, WorldJournal


def digest(data):
    return hashlib.sha256(data.encode()).hexdigest()


class DivergentSaveTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / "campaign.db"
        self.open_world((Stack("host", "sword:tempered:37", 1),))
        self.addCleanup(lambda: self.journal.db.close())
        self.register("host", "host-before", 0)
        self.register("guest", "guest-before", 0)

    def open_world(self, seed=()):
        self.journal = WorldJournal(self.path, "campaign-one", "manifest-one", seed)
        self.saves = SaveRecovery(self.journal)

    def register(self, holder, label, revision):
        self.saves.register_completed_save(holder, digest(label), revision)

    def plan(self, holder, label):
        return self.saves.plan_for_save(holder, digest(label))

    def move(self, request, actor, source, destination):
        return self.journal.commit(WorldAction(request, actor, (Transfer(source, destination, "sword:tempered:37", 1),)))

    def test_asymmetric_saves_restart_ack_loss_and_repeat(self):
        trades = PlayerTrades(self.journal)
        trades.invite("T", "host", "guest")
        trades.accept("T", "guest")
        trades.move("T", "deposit", "host", "host", "sword:tempered:37", 1, take=False)
        self.register("host", "host-after-deposit", 1)
        trades.move("T", "take", "guest", "host", "sword:tempered:37", 1, take=True)
        self.journal.db.close()
        self.open_world()
        trades = PlayerTrades(self.journal)
        trades.recover_on_server_start()
        retry = trades.move("T", "take", "guest", "host", "sword:tempered:37", 1, take=True)
        self.assertFalse(retry.applied)
        self.assertEqual(self.plan("host", "host-after-deposit"), RecoveryPlan(1, 2, ()))
        guest = self.plan("guest", "guest-before")
        self.assertEqual(guest, RecoveryPlan(0, 2, (Stack("guest", "sword:tempered:37", 1),)))
        self.assertEqual(self.plan("guest", "guest-before"), guest)
        self.assertEqual(self.plan("host", "host-before").stacks, (Stack("host", "sword:tempered:37", 0),))
        self.assertEqual(self.journal.snapshot().stacks, guest.stacks)

    def test_replay_old_save_after_item_spent_does_not_regrant_it(self):
        self.move("give", "host", "host", "guest")
        self.move("store", "guest", "guest", "chest:one")
        self.assertEqual(self.plan("guest", "guest-before").stacks, (Stack("guest", "sword:tempered:37", 0),))

    def test_refuse_foreign_unrecognized_and_conflicting_checkpoints(self):
        with self.assertRaises(Rejected):
            self.plan("guest", "host-before")
        with self.assertRaises(Rejected):
            self.plan("guest", "solo-save")
        with self.assertRaises(Rejected):
            self.register("host", "future", 42)
        self.register("host", "host-before", 0)
        self.move("give", "host", "host", "guest")
        with self.assertRaises(Rejected):
            self.register("host", "host-before", 1)

    def test_commits_completed_while_saving_are_replayed(self):
        self.move("give", "host", "host", "guest")
        self.register("guest", "finished-after-give", 0)
        self.assertEqual(self.plan("guest", "finished-after-give").stacks, (Stack("guest", "sword:tempered:37", 1),))


if __name__ == "__main__":
    unittest.main()
