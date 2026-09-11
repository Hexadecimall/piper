"""Core pseudo-random generator used by random."""

from os import urandom


class Random:
    VERSION = 1

    def __init__(self, seed=None):
        self.seed(seed)

    def seed(self, value=None):
        if value is None:
            value = 0
            for byte in urandom(16):
                value = (value << 8) | byte
        elif not isinstance(value, int):
            value = hash(value)
        self._state = value & ((1 << 64) - 1)
        if self._state == 0:
            self._state = 0x9e3779b97f4a7c15

    def _next(self):
        value = self._state
        value ^= value >> 12
        value ^= (value << 25) & ((1 << 64) - 1)
        value ^= value >> 27
        self._state = value
        return (value * 0x2545f4914f6cdd1d) & ((1 << 64) - 1)

    def random(self):
        return (self._next() >> 11) / 9007199254740992.0

    def getrandbits(self, bits):
        if bits < 0:
            raise ValueError("number of bits must be non-negative")
        value = 0
        produced = 0
        while produced < bits:
            take = min(64, bits - produced)
            value |= (self._next() & ((1 << take) - 1)) << produced
            produced += take
        return value

    def getstate(self):
        return self._state

    def setstate(self, state):
        self._state = state & ((1 << 64) - 1)

