from io import BytesIO
from pathlib import Path
import sqlite3
import tempfile
import unittest

from replica import GuestReplica, HostPublisher, ReplicaError
from reconnect import HostHistory, PeerLog, StaleRequest
from world_journal import Rejected, Stack, Transfer, WorldAction, WorldJournal


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.world = WorldJournal(self.root / "host.db", "c", "m", (Stack("H", "sword", 1),))
        self.addCleanup(self.world.db.close)
        self.publisher = HostPublisher(self.world, 60)
        self.replica = GuestReplica(self.root / "guest-copy", "c", "m")
        self.addCleanup(self.replica.db.close)
        self.action = WorldAction("give", "H", (Transfer("H", "G", "sword", 1),))
        self.history = HostHistory(self.world)

    def publish(self, now):
        sent = []
        def receive(cp, reader):
            data = reader.read()
            self.replica.receive(cp, BytesIO(data))
            sent.append((cp, data))
        self.publisher.publish_if_due(now, receive)
        return sent

    def test_interval_installed_image_and_late_copy(self):
        [(old, old_bytes)] = self.publish(0)
        self.assertFalse(self.publish(59))
        self.world.commit(self.action)
        self.publish(60)
        with sqlite3.connect(f"{self.replica.current().as_uri()}?mode=ro", uri=True) as saved:
            self.assertEqual(saved.execute("SELECT holder,quantity FROM stacks").fetchall(), [("G", 1)])
        self.publish(120)
        self.assertEqual(len(list((self.root / "guest-copy").glob("campaign-*.sqlite"))), 2)
        with self.assertRaises(ReplicaError):
            self.replica.receive(old, BytesIO(old_bytes))

    def test_interrupted_transfer_keeps_last_good_copy_and_retries(self):
        self.publish(0)
        last = self.replica.current()
        def truncated(cp, reader):
            self.replica.receive(cp, BytesIO(reader.read(1500)))
        with self.assertRaises(ReplicaError):
            self.publisher.publish_if_due(60, truncated)
        self.assertEqual(self.replica.current(), last)
        self.assertTrue(last.exists())
        self.assertFalse(list(last.parent.glob("*.partial")))
        self.assertTrue(self.publish(61))

    def test_fork_after_restoring_a_previous_host_db_is_not_silently_installed(self):
        [(first, raw)] = self.publish(0)
        self.publish(60)
        branch = self.root / "restored.db"
        branch.write_bytes(raw)
        with WorldJournal(branch, "c", "m") as other:
            publisher = HostPublisher(other)
            publisher.publish_if_due(0, lambda *_: None)
            with self.assertRaisesRegex(ReplicaError, "branches"):
                publisher.publish_if_due(60, self.replica.receive)

    def peer(self, who):
        peer = PeerLog(self.root / (who + ".log.db"), who, self.history)
        self.addCleanup(peer.db.close)
        return peer

    def test_both_disconnect_between_commit_and_ack_guest_crashes_before_checkpoint(self):
        h, g = self.peer("H"), self.peer("G")
        h.queue(self.action)
        self.world.commit(self.action)
        for peer in (h,g):
            peer.interrupted("transport_lost")
        no_retry = lambda action: self.fail("receipt already exists")
        h.reconcile(self.history, no_retry, lambda _: None)
        inventory = {}
        def interrupted_apply(event):
            inventory.update({s.item:s.quantity for s in event.stacks if s.holder == "G"})
            raise ConnectionError()
        with self.assertRaises(ConnectionError):
            g.reconcile(self.history,no_retry,interrupted_apply)
        self.assertEqual(g.cursor().revision,0)
        self.peer("G").reconcile(self.history,no_retry,
                 lambda e: inventory.update({s.item:s.quantity for s in e.stacks if s.holder == "G"}))
        self.assertEqual(inventory, {"sword":1})
        self.assertEqual(self.world.snapshot().revision,1)

    def test_missing_request_revalidated_and_stale_request_does_not_block_catchup(self):
        h = self.peer("H")
        h.queue(self.action)
        bad = WorldAction("old", "H", (Transfer("H","G","sword",1),))
        h.queue(bad)
        def validate(action):
            if action.action_id == "old":
                raise StaleRequest("changed stack")
            self.world.commit(action)
        observed = []
        h.reconcile(self.history,validate,observed.append)
        self.assertEqual(len(observed),1)
        self.assertEqual(h.db.execute("SELECT * FROM rejected").fetchall(), [("old","changed stack")])
        self.assertEqual(h.db.execute("SELECT * FROM outbox").fetchall(), [])
        with self.assertRaisesRegex(StaleRequest, "changed stack"):
            self.peer("H").queue(bad)

    def test_other_history_or_future_cursor_never_overwrites_authoritative_world(self):
        peer = self.peer("G")
        self.world.commit(self.action)
        peer.reconcile(self.history,self.world.commit,lambda _: None)
        with WorldJournal(self.root/"other.db", "c", "m") as other:
            with self.assertRaisesRegex(Rejected,"history"):
                peer.reconcile(HostHistory(other),other.commit,lambda _: None)
            self.assertEqual(other.snapshot().revision,0)
