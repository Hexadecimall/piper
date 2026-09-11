class DecimalException(ArithmeticError):
    pass


class InvalidOperation(DecimalException):
    pass


class DivisionByZero(DecimalException, ZeroDivisionError):
    pass


class Inexact(DecimalException):
    pass


class Rounded(DecimalException):
    pass


class Overflow(DecimalException, OverflowError):
    pass


class Underflow(DecimalException):
    pass


class Subnormal(DecimalException):
    pass


class Clamped(DecimalException):
    pass


ROUND_CEILING = "ROUND_CEILING"
ROUND_DOWN = "ROUND_DOWN"
ROUND_FLOOR = "ROUND_FLOOR"
ROUND_HALF_DOWN = "ROUND_HALF_DOWN"
ROUND_HALF_EVEN = "ROUND_HALF_EVEN"
ROUND_HALF_UP = "ROUND_HALF_UP"
ROUND_UP = "ROUND_UP"
ROUND_05UP = "ROUND_05UP"


class Context:
    def __init__(self, prec=28, rounding=ROUND_HALF_EVEN, Emin=-999999, Emax=999999, capitals=1, clamp=0, flags=None, traps=None):
        self.prec = prec
        self.rounding = rounding
        self.Emin = Emin
        self.Emax = Emax
        self.capitals = capitals
        self.clamp = clamp
        self.flags = {} if flags is None else dict(flags)
        self.traps = {} if traps is None else dict(traps)

    def copy(self):
        return Context(self.prec, self.rounding, self.Emin, self.Emax, self.capitals, self.clamp, self.flags, self.traps)


DefaultContext = Context()
BasicContext = Context(prec=9, rounding=ROUND_HALF_UP)
ExtendedContext = Context(prec=9)
_context = DefaultContext.copy()


def getcontext():
    return _context


def setcontext(context):
    global _context
    _context = context.copy()


class _LocalContext:
    def __init__(self, context, overrides):
        self.context = (getcontext() if context is None else context).copy()
        for name, value in overrides.items():
            setattr(self.context, name, value)

    def __enter__(self):
        self.previous = getcontext()
        setcontext(self.context)
        return getcontext()

    def __exit__(self, exc_type, exc, traceback):
        setcontext(self.previous)


def localcontext(ctx=None, **kwargs):
    return _LocalContext(ctx, kwargs)


def _parts(value):
    text = str(value).strip()
    sign = -1 if text.startswith("-") else 1
    if text[:1] in "+-":
        text = text[1:]
    if text.lower() in ("nan", "snan", "infinity", "inf"):
        return sign, text.lower(), 0
    if "e" in text.lower():
        mantissa, exponent_text = text.lower().split("e", 1)
        exponent = int(exponent_text)
    else:
        mantissa, exponent = text, 0
    if "." in mantissa:
        whole, fraction = mantissa.split(".", 1)
        exponent -= len(fraction)
        digits = whole + fraction
    else:
        digits = mantissa
    if not digits or not digits.isdigit():
        raise InvalidOperation(f"invalid decimal literal: {value!r}")
    return sign, int(digits or "0"), exponent


class Decimal:
    def __init__(self, value="0", context=None):
        if isinstance(value, Decimal):
            self._sign, self._coefficient, self._exponent = value._sign, value._coefficient, value._exponent
        else:
            self._sign, self._coefficient, self._exponent = _parts(value)

    @classmethod
    def _new(cls, coefficient, exponent):
        result = object.__new__(cls)
        result._sign = -1 if coefficient < 0 else 1
        result._coefficient = abs(coefficient)
        result._exponent = exponent
        return result

    def _signed(self):
        return self._sign * self._coefficient

    def _align(self, other):
        other = other if isinstance(other, Decimal) else Decimal(other)
        exponent = min(self._exponent, other._exponent)
        left = self._signed() * 10 ** (self._exponent - exponent)
        right = other._signed() * 10 ** (other._exponent - exponent)
        return left, right, exponent

    def __add__(self, other):
        left, right, exponent = self._align(other)
        return Decimal._new(left + right, exponent)

    __radd__ = __add__

    def __sub__(self, other):
        left, right, exponent = self._align(other)
        return Decimal._new(left - right, exponent)

    def __rsub__(self, other):
        return Decimal(other).__sub__(self)

    def __mul__(self, other):
        other = other if isinstance(other, Decimal) else Decimal(other)
        return Decimal._new(self._signed() * other._signed(), self._exponent + other._exponent)

    __rmul__ = __mul__

    def __neg__(self):
        return Decimal._new(-self._signed(), self._exponent)

    def __pos__(self):
        return Decimal(self)

    def __abs__(self):
        return Decimal._new(self._coefficient, self._exponent)

    def __bool__(self):
        return self._coefficient != 0

    def __eq__(self, other):
        try:
            left, right, exponent = self._align(other)
            return left == right
        except (InvalidOperation, ValueError, TypeError):
            return False

    def __hash__(self):
        return hash((self._signed(), self._exponent))

    def as_tuple(self):
        digits = tuple(int(character) for character in str(self._coefficient))
        return (1 if self._sign < 0 else 0, digits, self._exponent)

    def __str__(self):
        if isinstance(self._coefficient, str):
            return ("-" if self._sign < 0 else "") + self._coefficient
        digits = str(self._coefficient)
        if self._exponent >= 0:
            body = digits + "0" * self._exponent
        else:
            point = len(digits) + self._exponent
            body = digits[:point] + "." + digits[point:] if point > 0 else "0." + "0" * -point + digits
        return ("-" if self._sign < 0 and self._coefficient else "") + body

    def __repr__(self):
        return f"Decimal('{self}')"


DecimalTuple = tuple
MAX_PREC = 999999999999999999
MAX_EMAX = 999999999999999999
MIN_EMIN = -999999999999999999
MIN_ETINY = -1999999999999999997
HAVE_CONTEXTVAR = True
HAVE_THREADS = True
