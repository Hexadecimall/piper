"""Stream interfaces backed by Piper's native file objects."""

import builtins

DEFAULT_BUFFER_SIZE = 8192
SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2
open = builtins.open
open_code = builtins.open


class UnsupportedOperation(OSError):
    pass


class IOBase:
    def close(self):
        pass

    @property
    def closed(self):
        return False

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()


class RawIOBase(IOBase):
    pass


class BufferedIOBase(IOBase):
    pass


class TextIOBase(IOBase):
    pass


class StringIO(TextIOBase):
    def __init__(self, initial_value="", newline="\n"):
        self._value = initial_value
        self._position = 0
        self._closed = False

    @property
    def closed(self):
        return self._closed

    def close(self):
        self._closed = True

    def getvalue(self):
        if self._closed:
            raise ValueError("I/O operation on closed file")
        return self._value

    def tell(self):
        return self._position

    def seek(self, offset, whence=SEEK_SET):
        if whence == SEEK_SET:
            position = offset
        elif whence == SEEK_CUR:
            position = self._position + offset
        elif whence == SEEK_END:
            position = len(self._value) + offset
        else:
            raise ValueError("invalid whence")
        if position < 0:
            raise ValueError("negative seek position")
        self._position = position
        return position

    def write(self, value):
        if not isinstance(value, str):
            raise TypeError("string argument expected")
        before = self._value[:self._position]
        end = self._position + len(value)
        after = self._value[end:]
        if self._position > len(self._value):
            before += "\0" * (self._position - len(self._value))
        self._value = before + value + after
        self._position += len(value)
        return len(value)

    def read(self, size=-1):
        if size is None or size < 0:
            end = len(self._value)
        else:
            end = min(len(self._value), self._position + size)
        value = self._value[self._position:end]
        self._position = end
        return value

    def readline(self, size=-1):
        end = self._value.find("\n", self._position)
        if end < 0:
            end = len(self._value)
        else:
            end += 1
        if size is not None and size >= 0:
            end = min(end, self._position + size)
        value = self._value[self._position:end]
        self._position = end
        return value


class BytesIO(BufferedIOBase):
    def __init__(self, initial_bytes=b""):
        self._value = bytearray(initial_bytes)
        self._position = 0
        self._closed = False

    @property
    def closed(self):
        return self._closed

    def close(self):
        self._closed = True

    def getvalue(self):
        return bytes(self._value)

    def tell(self):
        return self._position

    def seek(self, offset, whence=SEEK_SET):
        if whence == SEEK_SET:
            position = offset
        elif whence == SEEK_CUR:
            position = self._position + offset
        elif whence == SEEK_END:
            position = len(self._value) + offset
        else:
            raise ValueError("invalid whence")
        if position < 0:
            raise ValueError("negative seek position")
        self._position = position
        return position

    def write(self, value):
        value = bytes(value)
        end = self._position + len(value)
        if end > len(self._value):
            self._value.extend(b"\0" * (end - len(self._value)))
        self._value[self._position:end] = value
        self._position = end
        return len(value)

    def read(self, size=-1):
        if size is None or size < 0:
            end = len(self._value)
        else:
            end = min(len(self._value), self._position + size)
        value = bytes(self._value[self._position:end])
        self._position = end
        return value


FileIO = RawIOBase
BufferedReader = BufferedIOBase
BufferedWriter = BufferedIOBase
BufferedRWPair = BufferedIOBase
BufferedRandom = BufferedIOBase
TextIOWrapper = TextIOBase
IncrementalNewlineDecoder = TextIOBase


def text_encoding(encoding, stacklevel=2):
    if encoding is None:
        return "locale"
    return encoding

