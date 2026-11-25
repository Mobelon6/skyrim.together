from pathlib import Path
import tempfile
import unittest

from homes import PlayerHomes
from world_journal import Rejected, Stack, WorldJournal


class HouseTests(unittest.TestCase):
    def test_same_ref_in_two_breezehomes_and_readonly_guest(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "campaign"
            with WorldJournal(path, "one", "mods", (Stack("A","gem",3), Stack("B","gem",1))) as db:
                h = PlayerHomes(db)
                self.assertEqual(h.choices("Breezehome", {"A", "B"}), ())
                h.record_purchase("Breezehome","A")
                h.record_purchase("Breezehome","B")
                self.assertEqual(h.choices("Breezehome", {"A", "B"}), ("A","B"))
                self.assertEqual(h.choices("Breezehome", {"A"}), ("A",))
                h.move("1","A","Breezehome","A","chest","gem",2,take=False)
                h.move("1","B","Breezehome","B","chest","gem",1,take=False)
                for take in (True, False):
                    with self.assertRaises(Rejected):
                        h.move("foreign","B","Breezehome","A","chest","gem",1,take=take)
                self.assertEqual(h.contents("Breezehome","A","chest"), (("gem",2),))
                self.assertEqual(h.contents("Breezehome","B","chest"), (("gem",1),))
            with WorldJournal(path, "one", "mods") as reopened:
                self.assertEqual(PlayerHomes(reopened).contents("Breezehome","A","chest"), (("gem",2),))
