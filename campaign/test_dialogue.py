from pathlib import Path
import tempfile
import unittest

from dialogue import DialogueParty, Effect
from world_journal import Rejected, Stack, Stage, Transfer, WorldJournal


class ConversationTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.db = WorldJournal(Path(folder.name,"world"),"world","profile",
                               (Stack("npc","gold",30), Stack("host","key",1)))
        self.addCleanup(self.db.db.close)
        self.party = DialogueParty(self.db,"pair",("host","guest"),"host")
        self.party.start("talk","guest","npc")

    def test_guest_speaks_both_receive_credit_shared_payment_and_personal_gate(self):
        p = self.party
        p.observe("talk","host",True)
        self.assertEqual(p.choose("talk","guest",1,"INFO-8"), ("guest","host"))
        p.observe("talk","host",False)
        rules = (Effect("pay","shared",(Transfer("host","npc","key",1),)),
                 Effect("door","shared",stage=Stage("access","run",50)),
                 Effect("reward","participant",(Transfer("npc","$recipient","gold",5),)),
                 Effect("recruitment","speaker",stage=Stage("personal","run",10)))
        self.assertTrue(p.confirm_outcome("talk",1,"host",1,rules))
        self.assertFalse(p.confirm_outcome("talk",1,"host",1,rules))
        self.assertEqual(self.db.snapshot().stacks, (Stack("guest","gold",5), Stack("host","gold",5),
                                                     Stack("npc","gold",20), Stack("npc","key",1)))
        self.assertEqual(len(self.db.snapshot().stages),2)
        self.assertEqual(p.choose("talk","guest",1,"INFO-8"), ("guest","host"))
        with self.assertRaises(Rejected):
            p.choose("talk","guest",1,"different-info")

    def test_late_listener_gets_no_retroactive_reward(self):
        p=self.party
        p.choose("talk","guest",1,"option")
        p.observe("talk","host",True)
        rule=(Effect("reward","participant",(Transfer("npc","$recipient","gold",1),)),)
        p.confirm_outcome("talk",1,"host",1,rule)
        p.choose("talk","guest",2,"option")
        p.confirm_outcome("talk",2,"host",1,rule)
        self.assertIn(Stack("host","gold",1),self.db.snapshot().stacks)
        self.assertIn(Stack("guest","gold",2),self.db.snapshot().stacks)

    def test_handoff_or_exit_does_not_rerun_an_accepted_choice(self):
        p=self.party
        p.choose("talk","guest",1,"entry")
        p.handoff("host","guest",1)
        p.close("talk","guest")
        with self.assertRaises(Rejected):
            p.confirm_outcome("talk",1,"host",1,())
        self.assertTrue(p.confirm_outcome("talk",1,"guest",2,()))
        self.assertFalse(p.confirm_outcome("talk",1,"guest",2,()))
        with self.assertRaises(Rejected):
            p.choose("talk","guest",2,"late")

    def test_listener_has_no_input_and_incomplete_effect_bundle_rolls_back(self):
        p=self.party
        p.observe("talk","host",True)
        with self.assertRaises(Rejected):
            p.choose("talk","host",1,"other")
        p.choose("talk","guest",1,"chosen")
        with self.assertRaises(Rejected):
            p.confirm_outcome("talk",1,"host",1,(Effect("one","shared",stage=Stage("quest","run",2)),
                                                  Effect("two","shared",(Transfer("host","npc","key",2),))))
        self.assertEqual(self.db.snapshot().revision,0)
