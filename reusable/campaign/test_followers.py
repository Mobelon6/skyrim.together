from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import tempfile
import unittest

from followers import FollowerReply, FollowerSlots
from world_journal import WorldJournal


class FollowerTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "world"
        self.db = WorldJournal(self.path, "one", "mods")
        self.addCleanup(self.db.db.close)
        self.followers = FollowerSlots(self.db)

    def test_individual_unlock_refusal_handoff_and_obsolete_dismiss(self):
        f = self.followers
        self.assertEqual(f.recruit("uthgerd","host",0,eligible=True), FollowerReply("companion_commands",1))
        self.assertEqual(f.dialogue("uthgerd","guest",eligible=False).option, "personal_prerequisites")
        self.assertEqual(f.recruit("uthgerd","guest",1,eligible=True).option, "follow_me_then_refuse")
        self.assertFalse(f.dismiss("uthgerd","guest",1))
        self.assertTrue(f.dismiss("uthgerd","host",1))
        self.assertEqual(f.recruit("uthgerd","guest",2,eligible=False).option, "personal_prerequisites")
        self.assertEqual(f.recruit("uthgerd","guest",2,eligible=True).option, "companion_commands")
        self.assertEqual(f.dialogue("lydia","guest",eligible=True).option, "follow_me_then_refuse")
        self.assertFalse(f.dismiss("uthgerd","host",1))
        with WorldJournal(self.path, "one", "mods") as reopened:
            self.assertEqual(FollowerSlots(reopened).dialogue("uthgerd","guest",eligible=False),
                             FollowerReply("companion_commands",3))

    def test_only_one_player_can_recruit_with_the_same_snapshot(self):
        def claim(character):
            with WorldJournal(self.path, "one", "mods") as db:
                return FollowerSlots(db).recruit("uthgerd", character, 0, eligible=True).option
        with ThreadPoolExecutor(2) as executor:
            replies = list(executor.map(claim, ("host", "guest")))
        self.assertCountEqual(replies, ["companion_commands", "follow_me_then_refuse"])
