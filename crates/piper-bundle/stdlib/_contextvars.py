class _Missing:
    def __repr__(self):
        return "<Token.MISSING>"


_MISSING = _Missing()


class Token:
    MISSING = _MISSING

    def __init__(self, context, variable, old_value):
        self._context = context
        self._variable = variable
        self._old_value = old_value
        self._used = False

    @property
    def var(self):
        return self._variable

    @property
    def old_value(self):
        return self._old_value


class ContextVar:
    def __init__(self, name, *, default=_MISSING):
        if not isinstance(name, str):
            raise TypeError("context variable name must be a str")
        self.name = name
        self._default = default

    def get(self, default=_MISSING):
        if self in _current._values:
            return _current._values[self]
        if default is not _MISSING:
            return default
        if self._default is not _MISSING:
            return self._default
        raise LookupError(self)

    def set(self, value):
        old = _current._values.get(self, _MISSING)
        _current._values[self] = value
        return Token(_current, self, old)

    def reset(self, token):
        if not isinstance(token, Token):
            raise TypeError("expected an instance of Token")
        if token._used:
            raise RuntimeError("Token has already been used once")
        if token._variable is not self:
            raise ValueError("Token was created by a different ContextVar")
        if token._context is not _current:
            raise ValueError("Token was created in a different Context")
        token._used = True
        if token._old_value is _MISSING:
            del _current._values[self]
        else:
            _current._values[self] = token._old_value

    def __repr__(self):
        if self._default is _MISSING:
            return f"<ContextVar name={self.name!r}>"
        return f"<ContextVar name={self.name!r} default={self._default!r}>"


class Context:
    def __init__(self):
        self._values = {}
        self._entered = False

    def copy(self):
        result = Context()
        result._values = self._values.copy()
        return result

    def run(self, callable, *args, **kwargs):
        global _current
        if self._entered:
            raise RuntimeError("cannot enter context: Context is already entered")
        previous = _current
        self._entered = True
        _current = self
        try:
            return callable(*args, **kwargs)
        finally:
            _current = previous
            self._entered = False

    def get(self, key, default=None):
        return self._values.get(key, default)

    def keys(self):
        return self._values.keys()

    def values(self):
        return self._values.values()

    def items(self):
        return self._values.items()

    def __contains__(self, key):
        return key in self._values

    def __getitem__(self, key):
        return self._values[key]

    def __iter__(self):
        return iter(self._values)

    def __len__(self):
        return len(self._values)


_current = Context()


def copy_context():
    return _current.copy()
