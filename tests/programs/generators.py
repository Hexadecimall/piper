def counter(limit):
    value = 0
    while value < limit:
        sent = yield value
        if sent is None:
            value += 1
        else:
            value = sent
    return "finished"


generator = counter(4)
print(type(generator).__name__, generator.gi_suspended)
print(next(generator), generator.gi_suspended)
print(generator.send(None))
print(generator.send(3))
try:
    next(generator)
except StopIteration as stopped:
    print(stopped.value)


def inner():
    sent = yield 11
    yield sent
    return 9


def outer():
    result = yield from inner()
    print("delegated", result)
    return result + 1


delegating = outer()
print(next(delegating))
print(delegating.send(5))
try:
    next(delegating)
except StopIteration as stopped:
    print("outer", stopped.value)


def expression_yield():
    value = 10 + (yield 2)
    return value


expression = expression_yield()
print(next(expression))
try:
    expression.send(7)
except StopIteration as stopped:
    print("expression", stopped.value)

closed = counter(2)
print(next(closed))
print(closed.close(), closed.gi_suspended)
print(list(closed))

factor = 3
generated = (value * factor for value in range(6) if value % 2)
print(type(generated).__name__, list(generated))
print(list((left, right) for left in range(3) for right in range(left)))
