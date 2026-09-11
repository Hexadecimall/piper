"""Conversions between binary data and ASCII encodings."""


class Error(ValueError):
    pass


class Incomplete(Error):
    pass


_HEX = b"0123456789abcdef"
_B64 = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def hexlify(data, sep=None, bytes_per_sep=1):
    raw = bytes(data)
    pieces = []
    for value in raw:
        pieces.append(bytes([_HEX[value >> 4], _HEX[value & 15]]))
    if sep is None:
        return b"".join(pieces)
    sep = bytes(sep)
    step = abs(bytes_per_sep)
    if step == 0:
        raise ValueError("bytes_per_sep must not be zero")
    groups = []
    if bytes_per_sep < 0:
        for index in range(0, len(pieces), step):
            groups.append(b"".join(pieces[index:index + step]))
    else:
        first = len(pieces) % step
        if first:
            groups.append(b"".join(pieces[:first]))
        for index in range(first, len(pieces), step):
            groups.append(b"".join(pieces[index:index + step]))
    return sep.join(groups)


b2a_hex = hexlify


def unhexlify(data):
    if isinstance(data, str):
        data = data.encode("ascii")
    data = bytes(data)
    if len(data) % 2:
        raise Error("Odd-length string")
    output = bytearray(len(data) // 2)
    digits = b"0123456789abcdef"
    for index in range(0, len(data), 2):
        left = bytes([data[index]]).lower()[0]
        right = bytes([data[index + 1]]).lower()[0]
        if left not in digits or right not in digits:
            raise Error("Non-hexadecimal digit found")
        output[index // 2] = digits.index(left) * 16 + digits.index(right)
    return bytes(output)


a2b_hex = unhexlify


def b2a_base64(data, newline=True):
    data = bytes(data)
    output = bytearray()
    for index in range(0, len(data), 3):
        chunk = data[index:index + 3]
        value = chunk[0] << 16
        if len(chunk) > 1:
            value |= chunk[1] << 8
        if len(chunk) > 2:
            value |= chunk[2]
        output.append(_B64[(value >> 18) & 63])
        output.append(_B64[(value >> 12) & 63])
        output.append(_B64[(value >> 6) & 63] if len(chunk) > 1 else 61)
        output.append(_B64[value & 63] if len(chunk) > 2 else 61)
    if newline:
        output.append(10)
    return bytes(output)


def a2b_base64(data, strict_mode=False):
    if isinstance(data, str):
        data = data.encode("ascii")
    cleaned = bytearray()
    for value in bytes(data):
        if value in _B64 or value == 61:
            cleaned.append(value)
        elif strict_mode:
            raise Error("Only base64 data is allowed")
    if len(cleaned) % 4:
        raise Error("Incorrect padding")
    output = bytearray()
    for index in range(0, len(cleaned), 4):
        chunk = cleaned[index:index + 4]
        values = []
        for value in chunk:
            values.append(0 if value == 61 else _B64.index(value))
        packed = values[0] << 18 | values[1] << 12 | values[2] << 6 | values[3]
        output.append((packed >> 16) & 255)
        if chunk[2] != 61:
            output.append((packed >> 8) & 255)
        if chunk[3] != 61:
            output.append(packed & 255)
    return bytes(output)


def crc32(data, value=0):
    crc = value ^ 0xffffffff
    for byte in bytes(data):
        crc ^= byte
        for unused in range(8):
            crc = (crc >> 1) ^ (0xedb88320 if crc & 1 else 0)
    return (crc ^ 0xffffffff) & 0xffffffff


def crc_hqx(data, value):
    crc = value
    for byte in bytes(data):
        crc ^= byte << 8
        for unused in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def a2b_qp(data, header=False):
    if isinstance(data, str):
        data = data.encode("ascii")
    output = bytearray()
    index = 0
    while index < len(data):
        value = data[index]
        if header and value == 95:
            output.append(32)
        elif value == 61 and index + 2 < len(data):
            if data[index + 1] in (10, 13):
                index += 1
                if data[index] == 13 and index + 1 < len(data) and data[index + 1] == 10:
                    index += 1
            else:
                output.extend(unhexlify(data[index + 1:index + 3]))
                index += 2
        else:
            output.append(value)
        index += 1
    return bytes(output)


def b2a_qp(data, quotetabs=False, istext=True, header=False):
    output = bytearray()
    for value in bytes(data):
        safe = 33 <= value <= 126 and value != 61
        if value == 32 and header:
            output.append(95)
        elif safe or value in (9, 32) and not quotetabs or istext and value in (10, 13):
            output.append(value)
        else:
            output.extend(b"=" + bytes([_HEX[value >> 4], _HEX[value & 15]]).upper())
    return bytes(output)

