"""Core collection types used by the Python standard library."""


def _index(value):
    if isinstance(value, int):
        return value
    method = getattr(value, "__index__", None)
    if method is None:
        raise TypeError("an integer is required")
    return method()


class deque:
    def __init__(self, iterable=(), maxlen=None):
        if maxlen is not None:
            maxlen = _index(maxlen)
            if maxlen < 0:
                raise ValueError("maxlen must be non-negative")
        self._items = []
        self._maxlen = maxlen
        self.extend(iterable)

    @property
    def maxlen(self):
        return self._maxlen

    def append(self, value):
        if self._maxlen == 0:
            return None
        self._items.append(value)
        if self._maxlen is not None and len(self._items) > self._maxlen:
            del self._items[0]

    def appendleft(self, value):
        if self._maxlen == 0:
            return None
        self._items.insert(0, value)
        if self._maxlen is not None and len(self._items) > self._maxlen:
            self._items.pop()

    def clear(self):
        self._items.clear()

    def copy(self):
        return type(self)(self, self._maxlen)

    __copy__ = copy

    def count(self, value):
        return self._items.count(value)

    def extend(self, iterable):
        if iterable is self:
            iterable = list(self._items)
        for value in iterable:
            self.append(value)

    def extendleft(self, iterable):
        if iterable is self:
            iterable = list(self._items)
        for value in iterable:
            self.appendleft(value)

    def index(self, value, start=0, stop=None):
        if stop is None:
            stop = len(self._items)
        return self._items.index(value, start, stop)

    def insert(self, index, value):
        if self._maxlen is not None and len(self._items) >= self._maxlen:
            raise IndexError("deque already at its maximum size")
        self._items.insert(_index(index), value)

    def pop(self):
        if not self._items:
            raise IndexError("pop from an empty deque")
        return self._items.pop()

    def popleft(self):
        if not self._items:
            raise IndexError("pop from an empty deque")
        return self._items.pop(0)

    def remove(self, value):
        self._items.remove(value)

    def reverse(self):
        self._items.reverse()

    def rotate(self, count=1):
        length = len(self._items)
        if length < 2:
            return None
        count = _index(count) % length
        if count:
            self._items[:] = self._items[-count:] + self._items[:-count]

    def __len__(self):
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __reversed__(self):
        return reversed(self._items)

    def __contains__(self, value):
        return value in self._items

    def __getitem__(self, index):
        return self._items[index]

    def __setitem__(self, index, value):
        self._items[index] = value

    def __delitem__(self, index):
        del self._items[index]

    def __add__(self, other):
        if not isinstance(other, deque):
            return NotImplemented
        return type(self)(list(self) + list(other), self._maxlen)

    def __mul__(self, count):
        return type(self)(self._items * _index(count), self._maxlen)

    __rmul__ = __mul__

    def __iadd__(self, other):
        self.extend(other)
        return self

    def __imul__(self, count):
        values = self._items * _index(count)
        self.clear()
        self.extend(values)
        return self

    def __eq__(self, other):
        if not isinstance(other, deque):
            return NotImplemented
        return self._items == other._items

    def __lt__(self, other):
        if not isinstance(other, deque):
            return NotImplemented
        return self._items < other._items

    def __le__(self, other):
        return self == other or self < other

    def __gt__(self, other):
        if not isinstance(other, deque):
            return NotImplemented
        return self._items > other._items

    def __ge__(self, other):
        return self == other or self > other

    def __repr__(self):
        name = type(self).__name__
        if self._maxlen is None:
            return "%s(%r)" % (name, self._items)
        return "%s(%r, maxlen=%r)" % (name, self._items, self._maxlen)

    def __reduce__(self):
        return type(self), (list(self), self._maxlen)


class defaultdict(dict):
    def __init__(self, default_factory=None, *args, **kwargs):
        if default_factory is not None and not callable(default_factory):
            raise TypeError("first argument must be callable or None")
        self.default_factory = default_factory
        super().__init__(*args, **kwargs)

    def __missing__(self, key):
        if self.default_factory is None:
            raise KeyError(key)
        value = self.default_factory()
        self[key] = value
        return value

    def copy(self):
        return type(self)(self.default_factory, self)

    __copy__ = copy

    def __reduce__(self):
        return type(self), (self.default_factory,), None, None, iter(self.items())

    def __repr__(self):
        return "defaultdict(%r, %s)" % (self.default_factory, dict.__repr__(self))


def _count_elements(mapping, iterable):
    get = mapping.get
    for element in iterable:
        mapping[element] = get(element, 0) + 1


class _deque_iterator:
    pass


class _deque_reverse_iterator:
    pass

