"""Core codec registry and stateless codecs for Piper."""

_search = []
_errors = {}


class _CodecInfo:
    def __init__(self, name, encode, decode):
        self.name = name
        self.encode = encode
        self.decode = decode
        self.incrementalencoder = None
        self.incrementaldecoder = None
        self.streamreader = None
        self.streamwriter = None

    def __getitem__(self, index):
        return (self.encode, self.decode, self.streamreader, self.streamwriter)[index]

    def __iter__(self):
        return iter((self.encode, self.decode, self.streamreader, self.streamwriter))


def _buffer(value):
    if isinstance(value, bytes):
        return value
    return bytes(value)


def _encoded(value, encoding, errors="strict"):
    result = value.encode(encoding, errors)
    return result, len(value)


def _decoded(value, encoding, errors="strict", final=False):
    value = _buffer(value)
    return value.decode(encoding, errors), len(value)


def utf_8_encode(value, errors="strict"):
    return _encoded(value, "utf-8", errors)


def utf_8_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-8", errors, final)


def ascii_encode(value, errors="strict"):
    return _encoded(value, "ascii", errors)


def ascii_decode(value, errors="strict"):
    return _decoded(value, "ascii", errors)


def latin_1_encode(value, errors="strict"):
    return _encoded(value, "latin-1", errors)


def latin_1_decode(value, errors="strict"):
    return _decoded(value, "latin-1", errors)


def utf_16_encode(value, errors="strict"):
    return _encoded(value, "utf-16", errors)


def utf_16_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-16", errors, final)


def utf_16_le_encode(value, errors="strict"):
    return _encoded(value, "utf-16-le", errors)


def utf_16_le_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-16-le", errors, final)


def utf_16_be_encode(value, errors="strict"):
    return _encoded(value, "utf-16-be", errors)


def utf_16_be_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-16-be", errors, final)


def utf_16_ex_decode(value, errors="strict", byteorder=0, final=False):
    decoded, consumed = _decoded(value, "utf-16-be" if byteorder > 0 else "utf-16-le" if byteorder < 0 else "utf-16", errors, final)
    return decoded, consumed, byteorder


def utf_32_encode(value, errors="strict"):
    return _encoded(value, "utf-32", errors)


def utf_32_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-32", errors, final)


def utf_32_le_encode(value, errors="strict"):
    return _encoded(value, "utf-32-le", errors)


def utf_32_le_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-32-le", errors, final)


def utf_32_be_encode(value, errors="strict"):
    return _encoded(value, "utf-32-be", errors)


def utf_32_be_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-32-be", errors, final)


def utf_32_ex_decode(value, errors="strict", byteorder=0, final=False):
    decoded, consumed = _decoded(value, "utf-32-be" if byteorder > 0 else "utf-32-le" if byteorder < 0 else "utf-32", errors, final)
    return decoded, consumed, byteorder


def utf_7_encode(value, errors="strict"):
    return _encoded(value, "utf-8", errors)


def utf_7_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-8", errors, final)


def readbuffer_encode(value, errors="strict"):
    value = _buffer(value)
    return value, len(value)


def escape_encode(value, errors="strict"):
    value = _buffer(value)
    result = bytearray()
    for byte in value:
        if byte == 9:
            result.extend(b"\\t")
        elif byte == 10:
            result.extend(b"\\n")
        elif byte == 13:
            result.extend(b"\\r")
        elif byte == 92:
            result.extend(b"\\\\")
        else:
            result.append(byte)
    return bytes(result), len(value)


def escape_decode(value, errors="strict"):
    value = _buffer(value)
    result = bytearray()
    index = 0
    escapes = {ord("n"): 10, ord("r"): 13, ord("t"): 9, ord("\\"): 92}
    while index < len(value):
        byte = value[index]
        if byte == 92 and index + 1 < len(value):
            index += 1
            byte = escapes.get(value[index], value[index])
        result.append(byte)
        index += 1
    return bytes(result), len(value)


def unicode_escape_encode(value, errors="strict"):
    return _encoded(value, "utf-8", errors)


def unicode_escape_decode(value, errors="strict", final=False):
    return _decoded(value, "utf-8", errors, final)


raw_unicode_escape_encode = unicode_escape_encode
raw_unicode_escape_decode = unicode_escape_decode


def charmap_build(table):
    return {character: index for index, character in enumerate(table)}


def charmap_encode(value, errors="strict", mapping=None):
    if mapping is None:
        return latin_1_encode(value, errors)
    result = bytearray()
    for character in value:
        mapped = mapping.get(ord(character), ord(character))
        if isinstance(mapped, int):
            result.append(mapped)
        else:
            result.extend(mapped)
    return bytes(result), len(value)


def charmap_decode(value, errors="strict", mapping=None):
    value = _buffer(value)
    if mapping is None:
        return latin_1_decode(value, errors)
    result = []
    for byte in value:
        mapped = mapping[byte]
        result.append(chr(mapped) if isinstance(mapped, int) else mapped)
    return "".join(result), len(value)


def register(search_function):
    _search.append(search_function)


def unregister(search_function):
    if search_function in _search:
        _search.remove(search_function)


def _normal(name):
    return name.lower().replace("-", "_").replace(" ", "_")


def lookup(name):
    normalized = _normal(name)
    builtins = {
        "utf_8": (utf_8_encode, utf_8_decode), "utf8": (utf_8_encode, utf_8_decode),
        "ascii": (ascii_encode, ascii_decode), "us_ascii": (ascii_encode, ascii_decode),
        "latin_1": (latin_1_encode, latin_1_decode), "latin1": (latin_1_encode, latin_1_decode),
        "utf_16": (utf_16_encode, utf_16_decode), "utf_16_le": (utf_16_le_encode, utf_16_le_decode), "utf_16_be": (utf_16_be_encode, utf_16_be_decode),
        "utf_32": (utf_32_encode, utf_32_decode), "utf_32_le": (utf_32_le_encode, utf_32_le_decode), "utf_32_be": (utf_32_be_encode, utf_32_be_decode),
    }
    if normalized in builtins:
        encoder, decoder = builtins[normalized]
        return _CodecInfo(normalized, encoder, decoder)
    for search_function in _search:
        result = search_function(normalized)
        if result is not None:
            return result
    raise LookupError("unknown encoding: " + name)


def encode(value, encoding="utf-8", errors="strict"):
    return lookup(encoding).encode(value, errors)[0]


def decode(value, encoding="utf-8", errors="strict"):
    return lookup(encoding).decode(value, errors)[0]


def register_error(name, handler):
    _errors[name] = handler


def _strict(error):
    raise error


def _ignore(error):
    return "", error.end


def _replace(error):
    return "?", error.end


def _xmlcharrefreplace(error):
    value = ""
    for character in error.object[error.start:error.end]:
        value += "&#" + str(ord(character)) + ";"
    return value, error.end


def _backslashreplace(error):
    value = ""
    for character in error.object[error.start:error.end]:
        code = ord(character)
        value += "\\x%02x" % code if code <= 255 else "\\u%04x" % code if code <= 65535 else "\\U%08x" % code
    return value, error.end


def _namereplace(error):
    return _backslashreplace(error)


def lookup_error(name):
    if name in _errors:
        return _errors[name]
    builtin = {"strict": _strict, "ignore": _ignore, "replace": _replace,
               "xmlcharrefreplace": _xmlcharrefreplace,
               "backslashreplace": _backslashreplace, "namereplace": _namereplace}
    if name in builtin:
        return builtin[name]
    raise LookupError("unknown error handler name " + name)


def _unregister_error(name):
    return _errors.pop(name, None) is not None
