from string.templatelib import Template


def generator():
    yield 1
    yield from [2, 3]
    return 9


async def source():
    for value in range(4):
        yield value


async def collect():
    return [value * 2 async for value in source() if value % 2]


name = "world"
template = t"hello {name!r:>8}"
print(isinstance(template, Template), template.strings)
print([(part.value, part.expression, part.conversion, part.format_spec) for part in template.interpolations])

iterator = generator()
print(next(iterator), next(iterator), next(iterator))
try:
    next(iterator)
except StopIteration as error:
    print(error.value)

coroutine = collect()
try:
    coroutine.send(None)
except StopIteration as error:
    print(error.value)
