"""Iterator building blocks for Piper."""

def count(start=0, step=1):
    while True:
        yield start
        start += step

def cycle(iterable):
    saved = []
    for item in iterable:
        yield item
        saved.append(item)
    while saved:
        for item in saved:
            yield item

def repeat(value, times=None):
    if times is None:
        while True:
            yield value
    else:
        for _ in range(times):
            yield value

def accumulate(iterable, func=None, *, initial=None):
    iterator = iter(iterable)
    if initial is None:
        try:
            total = next(iterator)
        except StopIteration:
            return
    else:
        total = initial
    yield total
    if func is None:
        for item in iterator:
            total += item
            yield total
    else:
        for item in iterator:
            total = func(total, item)
            yield total

def chain(*iterables):
    for iterable in iterables:
        for item in iterable:
            yield item

def _chain_from_iterable(iterable):
    for inner in iterable:
        for item in inner:
            yield item

chain.from_iterable = _chain_from_iterable

def compress(data, selectors):
    for item, selected in zip(data, selectors):
        if selected:
            yield item

def dropwhile(predicate, iterable):
    iterator = iter(iterable)
    for item in iterator:
        if not predicate(item):
            yield item
            break
    for item in iterator:
        yield item

def takewhile(predicate, iterable):
    for item in iterable:
        if not predicate(item):
            break
        yield item

def filterfalse(predicate, iterable):
    if predicate is None:
        predicate = bool
    for item in iterable:
        if not predicate(item):
            yield item

def starmap(function, iterable):
    for args in iterable:
        yield function(*args)

def islice(iterable, *args):
    if len(args) == 1:
        start, stop, step = 0, args[0], 1
    elif len(args) == 2:
        start, stop, step = args[0], args[1], 1
    elif len(args) == 3:
        start, stop, step = args
    else:
        raise TypeError("islice expected 2 to 4 arguments")
    if start is None:
        start = 0
    if step is None:
        step = 1
    if start < 0 or (stop is not None and stop < 0) or step <= 0:
        raise ValueError("indices for islice() must be non-negative and step greater than zero")
    for index, item in enumerate(iterable):
        if stop is not None and index >= stop:
            break
        if index >= start and (index - start) % step == 0:
            yield item

def pairwise(iterable):
    iterator = iter(iterable)
    try:
        previous = next(iterator)
    except StopIteration:
        return
    for item in iterator:
        yield previous, item
        previous = item

def batched(iterable, size, *, strict=False):
    if size < 1:
        raise ValueError("n must be at least one")
    iterator = iter(iterable)
    while True:
        batch = tuple(islice(iterator, size))
        if not batch:
            return
        if strict and len(batch) != size:
            raise ValueError("batched(): incomplete batch")
        yield batch

def product(*iterables, repeat=1):
    pools = [tuple(pool) for pool in iterables] * repeat
    result = [[]]
    for pool in pools:
        result = [prefix + [item] for prefix in result for item in pool]
    for values in result:
        yield tuple(values)

def permutations(iterable, length=None):
    pool = tuple(iterable)
    length = len(pool) if length is None else length
    if length < 0 or length > len(pool):
        return
    for indexes in product(range(len(pool)), repeat=length):
        if len(set(indexes)) == length:
            yield tuple(pool[index] for index in indexes)

def combinations(iterable, length):
    pool = tuple(iterable)
    def build(start, values):
        if len(values) == length:
            yield tuple(values)
            return
        for index in range(start, len(pool)):
            yield from build(index + 1, values + [pool[index]])
    yield from build(0, [])

def combinations_with_replacement(iterable, length):
    pool = tuple(iterable)
    def build(start, values):
        if len(values) == length:
            yield tuple(values)
            return
        for index in range(start, len(pool)):
            yield from build(index, values + [pool[index]])
    yield from build(0, [])

def zip_longest(*iterables, fillvalue=None):
    iterators = [iter(value) for value in iterables]
    active = len(iterators)
    while active:
        values = []
        active = 0
        for iterator in iterators:
            try:
                value = next(iterator)
                active += 1
            except StopIteration:
                value = fillvalue
            values.append(value)
        if active:
            yield tuple(values)

class groupby:
    def __init__(self, iterable, key=None):
        self.iterator = iter(iterable)
        self.key = (lambda value: value) if key is None else key
        self.pending = None
        self.finished = False

    def __iter__(self):
        return self

    def __next__(self):
        if self.finished:
            raise StopIteration
        if self.pending is None:
            try:
                first = next(self.iterator)
            except StopIteration:
                self.finished = True
                raise
        else:
            first = self.pending
            self.pending = None
        group_key = self.key(first)
        values = [first]
        for item in self.iterator:
            if self.key(item) != group_key:
                self.pending = item
                break
            values.append(item)
        else:
            self.finished = True
        return group_key, iter(values)
