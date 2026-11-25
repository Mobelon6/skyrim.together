"""One pending carriage departure. The game adapter owns the active trips and deadlines."""
from dataclasses import dataclass
from math import dist, isfinite

from world_journal import Rejected


@dataclass(frozen=True)
class Location:
    # A shared worldspace outdoors, or a specific interior / house instance indoors.
    space: str
    position: tuple[float, float, float]

    def __post_init__(self):
        if not self.space or len(self.position) != 3 or not all(map(isfinite, self.position)):
            raise Rejected("invalid location")


class CarriageTrip:
    def __init__(self, driver, party_at_departure, radius, expires_at):
        if driver not in party_at_departure or not isfinite(radius) or radius <= 0 or not isfinite(expires_at):
            raise Rejected("invalid carriage departure")
        origin = party_at_departure[driver]
        self.driver = driver
        self.expires_at = expires_at
        self.invited = frozenset(
            char for char, here in party_at_departure.items()
            if char != driver and here.space == origin.space and dist(here.position, origin.position) <= radius
        )
        self.answers = {}
        self.delivered = set()
        self.destination = None
        self.cancelled = False

    def _valid(self, now):
        if self.cancelled or now >= self.expires_at:
            raise Rejected("trip expired or cancelled")

    def answer(self, character, yes, now):
        self._valid(now)
        if character not in self.invited or type(yes) is not bool:
            raise Rejected("no invitation")
        if character in self.answers and self.answers[character] != yes:
            raise Rejected("invitation already answered")
        self.answers[character] = yes

    def arrive(self, initiator, destination, now):
        self._valid(now)
        if initiator != self.driver or not isinstance(destination, Location):
            raise Rejected("wrong departure or destination")
        if self.destination is not None and self.destination != destination:
            raise Rejected("arrival point already fixed")
        self.destination = destination

    def leave(self, character):
        if character == self.driver and self.destination is None:
            self.cancelled = True
        if character in self.invited:
            self.answers[character] = False

    def pending_moves(self, current_party, now):
        if self.cancelled or now >= self.expires_at or self.destination is None:
            return {}
        return {char: self.destination for char, yes in self.answers.items()
                if yes and char in current_party and char not in self.delivered}

    def acknowledge(self, character, current_party, now):
        if character in self.pending_moves(current_party, now):
            self.delivered.add(character)
