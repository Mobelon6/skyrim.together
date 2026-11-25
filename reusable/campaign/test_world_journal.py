from pathlib import Path
import sqlite3
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Barrier
from concurrent.futures import ThreadPoolExecutor
from unittest import TestCase, main

from world_journal import (
    MAX_STACK, Rejected, Stack, Stage, Transfer, WorldAction, WorldJournal,
)

WORLD = "host-campaign"
MANIFEST = "profile-hash"
GUEST = "player:guest"
HOST = "player:host"
NPC = "npc:merchant"
GROUND = "drop:10ba22a8"
SWORD = "Skyrim.esm:012ef4:iron"
ALE = "Skyrim.esm:09380d"
QUEST = "Skyrim.esm:09380b"


def open_world(path, initial_stacks=()):
    return WorldJournal(path, WORLD, MANIFEST, initial_stacks)


def counts(snapshot):
    return {(stack.holder, stack.item): stack.quantity for stack in snapshot.stacks}


class WorldJournalTests(TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "world.sqlite"
        with open_world(self.path, (
            Stack(HOST, SWORD, 1), Stack(NPC, ALE, 3),
            Stack("container:full", ALE, MAX_STACK),
        )):
            pass

    def test_drop_pickup_hides_drop_and_replays_once(self):
        drop = WorldAction("host/drop", HOST, (Transfer(HOST, GROUND, SWORD, 1),))
        pickup = self.pickup()
        with open_world(self.path) as world:
            first = world.commit(drop)
            pre_pickup = world.snapshot()
            second = world.commit(pickup)
            self.assertTrue(first.applied and second.applied)
            self.assertEqual(second.event.revision, pre_pickup.revision + 1)
            self.assertIn(Stack(GROUND, SWORD, 0), second.event.stacks)
            self.assertIn(Stack(GUEST, SWORD, 1), second.event.stacks)
            self.assertEqual(world.commit(pickup).event, second.event)
            self.assertFalse(world.commit(pickup).applied)
            self.assertEqual(world.events_after(first.event.revision), (second.event,))
            self.assertNotIn((GROUND, SWORD), counts(world.snapshot()))

            replica = counts(pre_pickup)
            revision = pre_pickup.revision
            for event in world.events_after(revision) * 2:
                if event.revision <= revision:
                    continue
                self.assertEqual(event.revision, revision + 1)
                for stack in event.stacks:
                    key = (stack.holder, stack.item)
                    if stack.quantity > 0:
                        replica[key] = stack.quantity
                    else:
                        replica.pop(key, None)
                revision = event.revision
            self.assertEqual(replica, counts(world.snapshot()))

    def test_process_exit_before_reply_retains_item_for_returning_guest(self):
        self.drop()
        script = f"""
import os
from pathlib import Path
from test_world_journal import open_world, WorldJournalTests
with open_world(Path({str(self.path)!r})) as world:
    world.commit(WorldJournalTests.pickup())
    os._exit(70)
"""
        result = subprocess.run(
            [sys.executable, "-c", script], cwd=Path(__file__).resolve().parent, check=False
        )
        self.assertEqual(result.returncode, 70)
        with open_world(self.path) as world:
            pickup = world.commit(self.pickup())
            self.assertFalse(pickup.applied)
            self.assertEqual(pickup.event.revision, 2)
            inventory = counts(world.snapshot())
            self.assertEqual(inventory[(GUEST, SWORD)], 1)
            self.assertNotIn((GROUND, SWORD), inventory)
            self.assertNotIn((HOST, SWORD), inventory)

    def test_two_simultaneous_pickups_leave_one_item(self):
        self.drop()
        barrier = Barrier(2, timeout=5)

        def try_pickup(player):
            with open_world(self.path) as world:
                barrier.wait()
                try:
                    result = world.commit(self.pickup(player))
                except Rejected as error:
                    self.assertEqual(str(error), "insufficient source quantity")
                    return None
                return result.event.actor

        with ThreadPoolExecutor(max_workers=2) as executor:
            outcomes = list(executor.map(try_pickup, (HOST, GUEST)))
        self.assertEqual(outcomes.count(None), 1)
        winner = next(player for player in outcomes if player is not None)
        with open_world(self.path) as world:
            sword_stacks = tuple(stack for stack in world.snapshot().stacks if stack.item == SWORD)
            self.assertEqual(sword_stacks, (Stack(winner, SWORD, 1),))

    def test_failure_between_debit_and_credit_rolls_back_both(self):
        self.drop()
        with open_world(self.path) as world:
            before = world.snapshot()
            world.db.execute("""
                CREATE TEMP TRIGGER fail_credit BEFORE INSERT ON stacks
                WHEN NEW.holder = 'player:guest'
                BEGIN
                    SELECT RAISE(ABORT, 'simulated failure before credit');
                END
            """)
            with self.assertRaises(sqlite3.IntegrityError):
                world.commit(self.pickup())
            self.assertEqual(world.snapshot(), before)
            self.assertEqual(world.events_after(before.revision), ())
            world.db.execute("DROP TRIGGER fail_credit")
            self.assertTrue(world.commit(self.pickup()).applied)

    def test_guest_quest_commit_group_duplicate_and_new_run(self):
        stage = Stage(QUEST, "run-1", 20)
        guest = WorldAction("guest/quest", GUEST, (Transfer(NPC, GUEST, ALE, 1),), stage)
        host = WorldAction("host/quest", HOST, (Transfer(NPC, HOST, ALE, 1),), stage)
        with open_world(self.path) as world:
            first = world.commit(guest)
            duplicate = world.commit(host)
            self.assertTrue(first.applied)
            self.assertFalse(duplicate.applied)
            self.assertEqual(duplicate.event, first.event)
            self.assertEqual(first.event.actor, GUEST)
            self.assertEqual(counts(world.snapshot())[(GUEST, ALE)], 1)
            self.assertNotIn((HOST, ALE), counts(world.snapshot()))
            lower = Stage(QUEST, "run-1", 10)
            new_run = Stage(QUEST, "run-2", 20)
            self.assertTrue(world.commit(WorldAction("guest/lower", GUEST, stage=lower)).applied)
            self.assertTrue(world.commit(WorldAction("host/repeat", HOST, stage=new_run)).applied)
            self.assertEqual(world.snapshot().stages, (stage, lower, new_run))

    def test_bad_quest_bundle_does_not_apply_stage_or_first_transfer(self):
        stage = Stage(QUEST, "run-1", 20)
        proposed = WorldAction("guest/quest", GUEST, (
            Transfer(NPC, GUEST, ALE, 1), Transfer(NPC, "container:full", ALE, 1),
        ), stage)
        with open_world(self.path) as world:
            before = world.snapshot()
            with self.assertRaisesRegex(Rejected, "destination stack is full"):
                world.commit(proposed)
            self.assertEqual(world.snapshot(), before)

    def test_changed_payload_under_same_operation_id_is_rejected(self):
        self.drop()
        with open_world(self.path) as world:
            world.commit(self.pickup())
            with self.assertRaisesRegex(Rejected, "another request"):
                world.commit(WorldAction(self.pickup().action_id, HOST, stage=Stage(QUEST, "r", 0)))

    def test_wrong_campaign_or_manifest_is_rejected_on_restart(self):
        with self.assertRaisesRegex(Rejected, "manifest mismatch"):
            WorldJournal(self.path, WORLD, "other-manifest")
        with self.assertRaisesRegex(Rejected, "manifest mismatch"):
            WorldJournal(self.path, "other-world", MANIFEST)

    def drop(self):
        with open_world(self.path) as world:
            world.commit(WorldAction("host/drop", HOST, (Transfer(HOST, GROUND, SWORD, 1),)))

    @staticmethod
    def pickup(player=GUEST):
        return WorldAction(f"{player}/pickup", player, (Transfer(GROUND, player, SWORD, 1),))


if __name__ == "__main__":
    main(verbosity=2)
