from typing import NoDefault


type Pair[T: object = int] = tuple[T, T]


def identity[T: int, U = str](value: T) -> T:
    return value


class Box[T: object = str]:
    def __init__(self, value: T):
        self.value = value


def variadic[*Ts, **P]():
    pass


print(Pair.__name__, Pair.__type_params__)
print(Pair.__value__)
print(identity.__type_params__, identity(7))
print(Box.__type_params__, Box("x").value)
for parameter in identity.__type_params__:
    print(parameter.__name__, parameter.__bound__, parameter.__default__, parameter.has_default())
print(identity.__type_params__[0].__default__ is NoDefault)
print(Pair.__type_params__[0].__bound__, Pair.__type_params__[0].__default__)
print(Box.__type_params__[0].__bound__, Box.__type_params__[0].__default__)
Ts, P = variadic.__type_params__
print(type(Ts).__name__, Ts.__name__, Ts.__default__, Ts.has_default())
print(type(P).__name__, P.__name__, P.__bound__, P.__default__, P.has_default())
print(type(P.args).__name__, P.args, P.args.__origin__ is P)
print(type(P.kwargs).__name__, P.kwargs, P.kwargs.__origin__ is P)
print(P.args is P.args, P.kwargs is P.kwargs)

try:
    raise ExceptionGroup("group", [ValueError("bad"), TypeError("wrong")])
except* ValueError as errors:
    print(type(errors).__name__, len(errors.exceptions), type(errors.exceptions[0]).__name__)
except* TypeError as errors:
    print(type(errors).__name__, len(errors.exceptions), type(errors.exceptions[0]).__name__)
