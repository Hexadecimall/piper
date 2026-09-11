"""CSV parser and writer used by the standard csv module."""

QUOTE_MINIMAL = 0
QUOTE_ALL = 1
QUOTE_NONNUMERIC = 2
QUOTE_NONE = 3
QUOTE_STRINGS = 4
QUOTE_NOTNULL = 5


class Error(Exception):
    pass


_dialects = {}
_field_limit = 131072


class Dialect:
    def __init__(self, dialect="excel", **settings):
        if isinstance(dialect, str):
            if dialect not in _dialects:
                raise Error("unknown dialect")
            dialect = _dialects[dialect]
        self.delimiter = settings.get("delimiter", getattr(dialect, "delimiter", ","))
        self.quotechar = settings.get("quotechar", getattr(dialect, "quotechar", '"'))
        self.escapechar = settings.get("escapechar", getattr(dialect, "escapechar", None))
        self.doublequote = settings.get("doublequote", getattr(dialect, "doublequote", True))
        self.skipinitialspace = settings.get("skipinitialspace", getattr(dialect, "skipinitialspace", False))
        self.lineterminator = settings.get("lineterminator", getattr(dialect, "lineterminator", "\r\n"))
        self.quoting = settings.get("quoting", getattr(dialect, "quoting", QUOTE_MINIMAL))
        self.strict = settings.get("strict", getattr(dialect, "strict", False))
        if not isinstance(self.delimiter, str) or len(self.delimiter) != 1:
            raise TypeError('"delimiter" must be a 1-character string')
        if self.quotechar is not None and (not isinstance(self.quotechar, str) or len(self.quotechar) != 1):
            raise TypeError('"quotechar" must be a 1-character string')
        if self.escapechar is not None and (not isinstance(self.escapechar, str) or len(self.escapechar) != 1):
            raise TypeError('"escapechar" must be a 1-character string')
        if self.quoting not in range(6):
            raise TypeError("bad quoting value")
        if self.quoting != QUOTE_NONE and self.quotechar is None:
            raise TypeError("quotechar must be set if quoting enabled")


def register_dialect(name, dialect="excel", **settings):
    if not isinstance(name, str):
        raise TypeError("dialect name must be a string")
    _dialects[name] = Dialect(dialect, **settings)


def unregister_dialect(name):
    if name not in _dialects:
        raise Error("unknown dialect")
    del _dialects[name]


def get_dialect(name):
    if name not in _dialects:
        raise Error("unknown dialect")
    return _dialects[name]


def list_dialects():
    return list(_dialects)


def field_size_limit(new_limit=None):
    global _field_limit
    old = _field_limit
    if new_limit is not None:
        _field_limit = int(new_limit)
    return old


def _parse_record(text, dialect):
    fields = []
    field = []
    quoted = False
    was_quoted = False
    index = 0
    while index < len(text):
        character = text[index]
        if quoted:
            if dialect.escapechar is not None and character == dialect.escapechar:
                index += 1
                if index < len(text):
                    field.append(text[index])
            elif character == dialect.quotechar:
                if dialect.doublequote and index + 1 < len(text) and text[index + 1] == dialect.quotechar:
                    field.append(character)
                    index += 1
                else:
                    quoted = False
            else:
                field.append(character)
        elif character == dialect.delimiter:
            value = "".join(field)
            fields.append(float(value) if dialect.quoting == QUOTE_NONNUMERIC and not was_quoted and value else value)
            field = []
            was_quoted = False
            if dialect.skipinitialspace and index + 1 < len(text) and text[index + 1] == " ":
                index += 1
        elif dialect.quotechar is not None and character == dialect.quotechar and not field:
            quoted = True
            was_quoted = True
        elif character in "\r\n":
            pass
        elif dialect.escapechar is not None and character == dialect.escapechar:
            index += 1
            if index < len(text):
                field.append(text[index])
        else:
            field.append(character)
        if len(field) > _field_limit:
            raise Error("field larger than field limit")
        index += 1
    if quoted:
        return None
    value = "".join(field)
    fields.append(float(value) if dialect.quoting == QUOTE_NONNUMERIC and not was_quoted and value else value)
    return fields


class _Reader:
    def __init__(self, iterable, dialect):
        self._iterator = iter(iterable)
        self.dialect = dialect
        self.line_num = 0

    def __iter__(self):
        return self

    def __next__(self):
        text = ""
        while True:
            line = next(self._iterator)
            if not isinstance(line, str):
                raise Error("iterator should return strings, not bytes")
            self.line_num += 1
            text += line
            result = _parse_record(text, self.dialect)
            if result is not None:
                return result


def reader(iterable, dialect="excel", **settings):
    return _Reader(iterable, Dialect(dialect, **settings))


class _Writer:
    def __init__(self, file, dialect):
        self._file = file
        self.dialect = dialect

    def _field(self, value):
        if value is None:
            text = ""
        else:
            text = str(value)
        quote = self.dialect.quoting == QUOTE_ALL
        quote = quote or self.dialect.quoting == QUOTE_NONNUMERIC and not isinstance(value, (int, float))
        quote = quote or self.dialect.quoting == QUOTE_STRINGS and isinstance(value, str)
        quote = quote or self.dialect.quoting == QUOTE_NOTNULL and value is not None
        special = self.dialect.delimiter in text or "\r" in text or "\n" in text
        if self.dialect.quotechar is not None and self.dialect.quotechar in text:
            special = True
            if self.dialect.doublequote:
                text = text.replace(self.dialect.quotechar, self.dialect.quotechar * 2)
            elif self.dialect.escapechar:
                text = text.replace(self.dialect.quotechar, self.dialect.escapechar + self.dialect.quotechar)
            else:
                raise Error("need to escape, but no escapechar set")
        quote = quote or self.dialect.quoting == QUOTE_MINIMAL and special
        if self.dialect.quoting == QUOTE_NONE and special:
            if not self.dialect.escapechar:
                raise Error("need to escape, but no escapechar set")
            text = text.replace(self.dialect.escapechar, self.dialect.escapechar * 2)
            text = text.replace(self.dialect.delimiter, self.dialect.escapechar + self.dialect.delimiter)
            text = text.replace("\r", self.dialect.escapechar + "\r").replace("\n", self.dialect.escapechar + "\n")
        if quote:
            text = self.dialect.quotechar + text + self.dialect.quotechar
        return text

    def writerow(self, row):
        text = self.dialect.delimiter.join(self._field(value) for value in row) + self.dialect.lineterminator
        return self._file.write(text)

    def writerows(self, rows):
        for row in rows:
            self.writerow(row)


def writer(file, dialect="excel", **settings):
    return _Writer(file, Dialect(dialect, **settings))

