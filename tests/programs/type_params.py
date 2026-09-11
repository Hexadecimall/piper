type Pair[T] = tuple[T, T]


def identity[T](value: T) -> T:
    return value


class Box[T]:
    def __init__(self, value: T):
        self.value = value


print(Pair.__name__, Pair.__type_params__)
print(Pair.__value__)
print(identity.__type_params__, identity(7))
print(Box.__type_params__, Box("x").value)

try:
    raise ExceptionGroup("group", [ValueError("bad"), TypeError("wrong")])
except* ValueError as errors:
    print(type(errors).__name__, len(errors.exceptions), type(errors.exceptions[0]).__name__)
except* TypeError as errors:
    print(type(errors).__name__, len(errors.exceptions), type(errors.exceptions[0]).__name__)
