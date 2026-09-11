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


def _round_quotient(numerator, denominator, rounding, negative=False):
    quotient, remainder = divmod(numerator, denominator)
    if not remainder:
        return quotient
    if rounding == ROUND_DOWN:
        return quotient
    if rounding == ROUND_UP:
        return quotient + 1
    if rounding == ROUND_CEILING:
        return quotient if negative else quotient + 1
    if rounding == ROUND_FLOOR:
        return quotient + 1 if negative else quotient
    if rounding == ROUND_05UP:
        return quotient + (quotient % 10 in (0, 5))
    doubled = remainder * 2
    if rounding == ROUND_HALF_UP:
        return quotient + (doubled >= denominator)
    if rounding == ROUND_HALF_DOWN:
        return quotient + (doubled > denominator)
    return quotient + (doubled > denominator or doubled == denominator and quotient % 2 == 1)


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

    def __truediv__(self, other):
        other = other if isinstance(other, Decimal) else Decimal(other)
        if other._coefficient == 0:
            raise DivisionByZero("division by zero")
        if self._coefficient == 0:
            return Decimal._new(0, -getcontext().prec)
        precision = getcontext().prec
        exponent = self._exponent - other._exponent
        length_delta = len(str(self._coefficient)) - len(str(other._coefficient))
        adjusted = length_delta + exponent
        if length_delta >= 0:
            if self._coefficient < other._coefficient * 10 ** length_delta:
                adjusted -= 1
        elif self._coefficient * 10 ** -length_delta < other._coefficient:
            adjusted -= 1
        scale = precision - 1 - adjusted
        power = exponent + scale
        if power >= 0:
            numerator = self._coefficient * 10 ** power
            denominator = other._coefficient
        else:
            numerator = self._coefficient
            denominator = other._coefficient * 10 ** -power
        negative = self._sign != other._sign
        coefficient = _round_quotient(numerator, denominator, getcontext().rounding, negative)
        return Decimal._new(-coefficient if negative else coefficient, -scale)

    def __rtruediv__(self, other):
        return Decimal(other).__truediv__(self)

    def __floordiv__(self, other):
        other = other if isinstance(other, Decimal) else Decimal(other)
        left, right, exponent = self._align(other)
        if right == 0:
            raise DivisionByZero("division by zero")
        quotient = abs(left) // abs(right)
        if left * right < 0:
            quotient = -quotient
        return Decimal(quotient)

    def __rfloordiv__(self, other):
        return Decimal(other).__floordiv__(self)

    def __mod__(self, other):
        other = other if isinstance(other, Decimal) else Decimal(other)
        quotient = self // other
        return self - quotient * other

    def __rmod__(self, other):
        return Decimal(other).__mod__(self)

    def __divmod__(self, other):
        quotient = self // other
        return quotient, self - quotient * Decimal(other)

    def __pow__(self, other, modulo=None):
        if modulo is not None:
            return Decimal(pow(int(self), int(other), int(modulo)))
        exponent = int(other)
        if Decimal(exponent) != Decimal(other):
            raise InvalidOperation("non-integral exponent")
        if exponent < 0:
            return Decimal(1) / (self ** -exponent)
        return Decimal._new(self._signed() ** exponent, self._exponent * exponent)

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

    def _compare(self, other):
        left, right, exponent = self._align(other)
        return -1 if left < right else 1 if left > right else 0

    def __lt__(self, other):
        return self._compare(other) < 0

    def __le__(self, other):
        return self._compare(other) <= 0

    def __gt__(self, other):
        return self._compare(other) > 0

    def __ge__(self, other):
        return self._compare(other) >= 0

    def __hash__(self):
        return hash((self._signed(), self._exponent))

    def as_tuple(self):
        digits = tuple(int(character) for character in str(self._coefficient))
        return (1 if self._sign < 0 else 0, digits, self._exponent)

    def adjusted(self):
        return self._exponent + len(str(self._coefficient)) - 1

    def is_finite(self):
        return not isinstance(self._coefficient, str)

    def is_infinite(self):
        return self._coefficient in ("infinity", "inf")

    def is_nan(self):
        return self._coefficient in ("nan", "snan")

    def is_qnan(self):
        return self._coefficient == "nan"

    def is_snan(self):
        return self._coefficient == "snan"

    def is_zero(self):
        return self._coefficient == 0

    def is_signed(self):
        return self._sign < 0

    def copy_abs(self):
        return Decimal._new(self._coefficient, self._exponent)

    def copy_negate(self):
        return Decimal._new(-self._signed(), self._exponent)

    def copy_sign(self, other):
        other = other if isinstance(other, Decimal) else Decimal(other)
        return Decimal._new(-self._coefficient if other._sign < 0 else self._coefficient, self._exponent)

    def normalize(self, context=None):
        if not self.is_finite() or self._coefficient == 0:
            return Decimal(0)
        coefficient = self._signed()
        exponent = self._exponent
        while coefficient % 10 == 0:
            coefficient //= 10
            exponent += 1
        return Decimal._new(coefficient, exponent)

    def quantize(self, exp, rounding=None, context=None):
        target = exp if isinstance(exp, Decimal) else Decimal(exp)
        wanted = target._exponent
        if self._exponent >= wanted:
            return Decimal._new(self._signed() * 10 ** (self._exponent - wanted), wanted)
        divisor = 10 ** (wanted - self._exponent)
        coefficient = _round_quotient(abs(self._signed()), divisor, rounding or (context or getcontext()).rounding, self._sign < 0)
        return Decimal._new(-coefficient if self._sign < 0 else coefficient, wanted)

    def to_integral_value(self, rounding=None, context=None):
        return self.quantize(Decimal(1), rounding, context)

    def to_integral(self, rounding=None, context=None):
        return self.to_integral_value(rounding, context)

    def __int__(self):
        if self._exponent >= 0:
            return self._signed() * 10 ** self._exponent
        return self._signed() // 10 ** -self._exponent if self._sign > 0 else -(self._coefficient // 10 ** -self._exponent)

    def __float__(self):
        return float(str(self))

    def __round__(self, ndigits=None):
        if ndigits is None:
            return int(self.to_integral_value())
        return self.quantize(Decimal._new(1, -ndigits))

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


Context.create_decimal = lambda self, value="0": Decimal(value, self)
Context.add = lambda self, left, right: Decimal(left) + Decimal(right)
Context.subtract = lambda self, left, right: Decimal(left) - Decimal(right)
Context.multiply = lambda self, left, right: Decimal(left) * Decimal(right)
Context.divide = lambda self, left, right: Decimal(left) / Decimal(right)
Context.quantize = lambda self, value, exp: Decimal(value).quantize(exp, context=self)


DecimalTuple = tuple
MAX_PREC = 999999999999999999
MAX_EMAX = 999999999999999999
MIN_EMIN = -999999999999999999
MIN_ETINY = -1999999999999999997
HAVE_CONTEXTVAR = True
HAVE_THREADS = True
