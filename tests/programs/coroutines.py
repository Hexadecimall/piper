async def child():
    return 5


async def parent():
    return await child() + 1


coroutine = parent()
print(type(coroutine).__name__)
iterator = coroutine.__await__()
try:
    next(iterator)
except StopIteration as stopped:
    print(stopped.value)


async def async_values(limit):
    for value in range(limit):
        yield value * 2


async def collect_generator():
    values = []
    async for value in async_values(4):
        values.append(value)
    return values


generator = async_values(1)
print(type(generator).__name__)
closer = generator.aclose().__await__()
try:
    next(closer)
except StopIteration:
    print("closed")
iterator = collect_generator().__await__()
try:
    next(iterator)
except StopIteration as stopped:
    print(stopped.value)


class AsyncContext:
    def __init__(self, suppress=False):
        self.suppress = suppress

    async def __aenter__(self):
        return "entered"

    async def __aexit__(self, exc_type, exc, traceback):
        print("exit", None if exc_type is None else exc_type.__name__)
        return self.suppress


async def contexts():
    async with AsyncContext() as value:
        print(value)
    async with AsyncContext(True):
        raise ValueError("hidden")
    return "contexts done"


iterator = contexts().__await__()
try:
    next(iterator)
except StopIteration as stopped:
    print(stopped.value)


class AsyncCounter:
    def __init__(self, limit):
        self.value = 0
        self.limit = limit

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self.value >= self.limit:
            raise StopAsyncIteration
        value = self.value
        self.value += 1
        return value


async def collect():
    values = []
    async for value in AsyncCounter(4):
        values.append(value)
    else:
        values.append("done")
    return values


iterator = collect().__await__()
try:
    next(iterator)
except StopIteration as stopped:
    print(stopped.value)


async def comprehensions():
    values = [value * 2 async for value in AsyncCounter(6) if value % 2]
    unique = {value async for value in AsyncCounter(4) if value > 1}
    mapping = {value: value + 10 async for value in AsyncCounter(3)}
    generated = (value + 20 async for value in AsyncCounter(3))
    gathered = [value async for value in generated]
    return values, unique, mapping, gathered


iterator = comprehensions().__await__()
try:
    next(iterator)
except StopIteration as stopped:
    print(stopped.value)


class Pause:
    def __await__(self):
        received = yield 8
        return received


async def waiting():
    return await Pause()


coroutine = waiting()
iterator = coroutine.__await__()
print(next(iterator))
try:
    iterator.send(12)
except StopIteration as stopped:
    print(stopped.value)
