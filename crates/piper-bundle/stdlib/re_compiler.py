"""Piper bridge from Python's regex parser to its execution engine."""

import _sre
from . import _parser
from ._constants import *


def isstring(value):
    return isinstance(value, (str, bytes))


def compile(pattern, flags=0):
    if isstring(pattern):
        source = pattern
        parsed = _parser.parse(pattern, flags)
    else:
        source = None
        parsed = pattern
    groupindex = parsed.state.groupdict
    indexgroup = [None] * parsed.state.groups
    for name, index in groupindex.items():
        indexgroup[index] = name
    return _sre.compile(source, flags | parsed.state.flags, parsed,
                        parsed.state.groups - 1, groupindex, tuple(indexgroup))

