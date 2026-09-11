"""Binary packing compatible with the public struct module."""

import sys


class error(Exception):
    pass


_SIZES = {
    "x": 1, "c": 1, "b": 1, "B": 1, "?": 1,
    "h": 2, "H": 2, "i": 4, "I": 4, "l": 4, "L": 4,
    "q": 8, "Q": 8, "n": 8, "N": 8, "P": 8,
    "e": 2, "f": 4, "d": 8,
}


def _layout(format):
    if not isinstance(format, str):
        format = format.decode("ascii")
    prefix = "@"
    if format and format[0] in "@=<>!":
        prefix, format = format[0], format[1:]
    endian = "big" if prefix in ">!" else "little" if prefix == "<" else sys.byteorder
    aligned = prefix == "@"
    fields = []
    offset = 0
    count = ""
    for code in format:
        if code.isspace():
            continue
        if code.isdigit():
            count += code
            continue
        repeat = int(count) if count else 1
        count = ""
        if code in "sp":
            size = repeat
            repeat = 1
        else:
            size = _SIZES.get(code)
            if size is None:
                raise error("bad char in struct format")
            if code in "nNP" and prefix != "@":
                raise error("bad char in struct format")
        if aligned and size > 1:
            alignment = min(size, 8)
            offset = (offset + alignment - 1) // alignment * alignment
        fields.append((offset, repeat, code, size, endian))
        offset += repeat * size
    if count:
        raise error("repeat count given without format specifier")
    return fields, offset


def calcsize(format):
    return _layout(format)[1]


def _integer(value, size, signed, endian):
    try:
        return int(value).to_bytes(size, endian, signed=signed)
    except OverflowError:
        raise error("argument out of range")


def pack(format, *values):
    fields, size = _layout(format)
    output = bytearray(size)
    value_index = 0
    for offset, repeat, code, width, endian in fields:
        if code == "x":
            continue
        if code in "sp":
            if value_index >= len(values):
                raise error("pack expected more items for packing")
            value = values[value_index]
            value_index += 1
            if not isinstance(value, (bytes, bytearray)):
                raise error("argument for 's' must be a bytes object")
            if code == "s":
                raw = bytes(value[:width])
                raw += b"\0" * (width - len(raw))
            else:
                length = min(len(value), max(0, width - 1), 255)
                raw = bytes([length]) + bytes(value[:length])
                raw += b"\0" * (width - len(raw))
            for index, byte in enumerate(raw):
                output[offset + index] = byte
            continue
        for item in range(repeat):
            if value_index >= len(values):
                raise error("pack expected more items for packing")
            value = values[value_index]
            value_index += 1
            if code == "c":
                if not isinstance(value, bytes) or len(value) != 1:
                    raise error("char format requires a bytes object of length 1")
                raw = value
            elif code == "?":
                raw = bytes([1 if value else 0])
            elif code in "bhiqn":
                raw = _integer(value, width, True, endian)
            elif code in "BHILQNP":
                raw = _integer(value, width, False, endian)
            else:
                raise error("floating-point formats are not available")
            start = offset + item * width
            for index, byte in enumerate(raw):
                output[start + index] = byte
    if value_index != len(values):
        raise error("pack expected fewer items for packing")
    return bytes(output)


def unpack(format, buffer):
    fields, size = _layout(format)
    if len(buffer) != size:
        raise error("unpack requires a buffer of %d bytes" % size)
    values = []
    for offset, repeat, code, width, endian in fields:
        if code == "x":
            continue
        if code == "s":
            values.append(bytes(buffer[offset:offset + width]))
            continue
        if code == "p":
            length = min(buffer[offset], max(0, width - 1))
            values.append(bytes(buffer[offset + 1:offset + 1 + length]))
            continue
        for item in range(repeat):
            start = offset + item * width
            raw = bytes(buffer[start:start + width])
            if code == "c":
                value = raw
            elif code == "?":
                value = raw[0] != 0
            elif code in "bhiqn":
                value = int.from_bytes(raw, endian, signed=True)
            elif code in "BHILQNP":
                value = int.from_bytes(raw, endian, signed=False)
            else:
                raise error("floating-point formats are not available")
            values.append(value)
    return tuple(values)


def pack_into(format, buffer, offset, *values):
    raw = pack(format, *values)
    if offset < 0:
        offset += len(buffer)
    if offset < 0 or offset + len(raw) > len(buffer):
        raise error("pack_into requires a buffer large enough")
    for index, byte in enumerate(raw):
        buffer[offset + index] = byte


def unpack_from(format, buffer, offset=0):
    size = calcsize(format)
    if offset < 0:
        offset += len(buffer)
    if offset < 0 or offset + size > len(buffer):
        raise error("unpack_from requires a buffer large enough")
    return unpack(format, buffer[offset:offset + size])


def iter_unpack(format, buffer):
    size = calcsize(format)
    if size == 0:
        raise error("cannot iteratively unpack with a struct of length 0")
    if len(buffer) % size:
        raise error("iterative unpacking requires a multiple of the format size")
    return iter([unpack_from(format, buffer, offset) for offset in range(0, len(buffer), size)])


def _clearcache():
    pass


class Struct:
    def __init__(self, format):
        self.format = format
        self.size = calcsize(format)

    def pack(self, *values):
        return pack(self.format, *values)

    def pack_into(self, buffer, offset, *values):
        return pack_into(self.format, buffer, offset, *values)

    def unpack(self, buffer):
        return unpack(self.format, buffer)

    def unpack_from(self, buffer, offset=0):
        return unpack_from(self.format, buffer, offset)

    def iter_unpack(self, buffer):
        return iter_unpack(self.format, buffer)

