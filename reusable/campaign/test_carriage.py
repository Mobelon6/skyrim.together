import unittest
from carriage import CarriageTrip, Location
from world_journal import Rejected


class CarriageTests(unittest.TestCase):
    def setUp(self):
        self.party = {"guest": Location("Tamriel",(5,0,0)), "host": Location("Tamriel",(0,0,0)),
                      "near": Location("Tamriel",(4,0,0)), "far": Location("Tamriel",(15,0,0)),
                      "inside": Location("interior:whiterun",(5,0,0))}
        self.trip = CarriageTrip("guest", self.party, 5, 20)
        self.destination = Location("Tamriel",(40000,10000,1200))

    def test_guest_drives_agreement_or_refusal_and_frozen_arrival(self):
        self.assertEqual(self.trip.invited, {"host","near"})
        self.trip.answer("host", True, 3)
        self.assertFalse(self.trip.pending_moves(self.party, 4))
        self.trip.arrive("guest", self.destination, 8)
        self.trip.answer("near", False, 10)
        self.assertEqual(self.trip.pending_moves(self.party, 10), {"host": self.destination})
        with self.assertRaises(Rejected):
            self.trip.arrive("guest", Location("Solitude",(0,0,0)), 11)
        self.trip.acknowledge("host", self.party, 11)
        self.assertFalse(self.trip.pending_moves(self.party, 12))

    def test_late_consent_departure_roster_expiry_and_cancellation(self):
        self.trip.arrive("guest", self.destination, 8)
        for char in ("far", "inside", "late"):
            with self.assertRaises(Rejected):
                self.trip.answer(char,True,10)
        self.trip.answer("host",True,11)
        self.trip.leave("host")
        self.assertFalse(self.trip.pending_moves(self.party,13))
        self.trip.answer("near",True,14)
        self.assertFalse(self.trip.pending_moves(self.party,20))
        another = CarriageTrip("host",self.party,20,30)
        another.leave("host")
        with self.assertRaises(Rejected):
            another.arrive("host",self.destination,15)
