import tempfile
from pathlib import Path
import unittest

from trade import Offer, PlayerTrades
from world_journal import Rejected, Stack, WorldJournal


class PlayerTradeTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name) / "world"
        self.open_world((Stack("A", "arrow", 7), Stack("B", "arrow", 3)))
        self.addCleanup(lambda: self.journal.db.close())
        self.trade.invite("T", "A", "B")

    def open_world(self, seed=()):
        self.journal = WorldJournal(self.path, "world", "mods", seed)
        self.trade = PlayerTrades(self.journal)

    def test_live_shared_buffer_both_directions_withdrawals_and_disconnect(self):
        self.trade.accept("T", "B")
        self.trade.move("T", "1", "A", "A", "arrow", 4, take=False)
        self.trade.move("T", "1", "B", "B", "arrow", 2, take=False)
        offers = (Offer("A", "arrow", 4), Offer("B", "arrow", 2))
        self.assertEqual(self.trade.offers("T", "A"), offers)
        self.assertEqual(self.trade.offers("T", "B"), offers)
        self.trade.move("T", "2", "A", "B", "arrow", 1, take=True)
        self.trade.move("T", "2", "B", "A", "arrow", 3, take=True)
        self.journal.db.close()
        self.open_world()
        self.assertEqual(self.trade.offers("T", "A"), (Offer("A", "arrow", 1), Offer("B", "arrow", 1)))
        self.trade.recover_on_server_start()
        self.trade.recover_on_server_start()
        self.trade.close("T", "B")
        self.assertFalse(self.trade.move("T", "2", "B", "A", "arrow", 3, take=True).applied)
        self.assertEqual(self.journal.snapshot().stacks, (Stack("A", "arrow", 5), Stack("B", "arrow", 5)))
        with self.assertRaises(Rejected):
            self.trade.move("T", "3", "B", "A", "arrow", 1, take=True)

    def test_decline_participants_and_busy_menu(self):
        with self.assertRaises(Rejected):
            self.trade.accept("T", "A")
        with self.assertRaises(Rejected):
            self.trade.accept("T", "C")
        with self.assertRaises(Rejected):
            self.trade.invite("another", "B", "C")
        with self.assertRaises(Rejected):
            self.trade.move("T", "early", "A", "A", "arrow", 1, take=False)
        self.trade.close("T", "B")
        self.assertEqual(self.trade.invite("T", "A", "B"), "closed")
        self.assertEqual(self.trade.invite("another", "B", "C"), "invited")
        self.assertEqual(self.journal.snapshot().revision, 0)

    def test_cannot_overdraw_duplicate_or_change_other_inventory(self):
        self.trade.accept("T", "B")
        with self.assertRaises(Rejected):
            self.trade.move("T", "foreign", "B", "A", "arrow", 1, take=False)
        self.trade.move("T", "deposit", "A", "A", "arrow", 1, take=False)
        self.trade.move("T", "grab", "B", "A", "arrow", 1, take=True)
        with self.assertRaises(Rejected):
            self.trade.move("T", "grab", "B", "A", "arrow", 2, take=True)
        with self.assertRaises(Rejected):
            self.trade.move("T", "also-grab", "A", "A", "arrow", 1, take=True)
        self.assertEqual(self.journal.snapshot().stacks, (Stack("A", "arrow", 6), Stack("B", "arrow", 4)))
