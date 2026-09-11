calls = 0


def resolve():
    global calls
    calls += 1
    return tuple[int, str]


type Result = resolve()
print(Result, Result.__name__, Result.__type_params__, calls)
print(Result.__value__, calls)
print(Result.__value__, calls)
from typing import TypeAliasType
print(type(Result) is TypeAliasType)

type Pair[T] = tuple[T, T]
print(Pair, Pair.__type_params__)
print(Pair.__value__)

type Callback[**P] = tuple[P]
type Packed[*Ts] = tuple[Ts]
print(Callback.__type_params__, type(Callback.__type_params__[0]).__name__)
print(Packed.__type_params__, type(Packed.__type_params__[0]).__name__)
