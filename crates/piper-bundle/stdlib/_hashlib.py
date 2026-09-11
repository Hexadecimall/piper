algorithms_available = {"md5", "sha1", "sha224", "sha256"}
openssl_md_meth_names = algorithms_available


_K256 = (
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
)


def _rotate(value, count):
    return ((value >> count) | (value << (32 - count))) & 0xffffffff


def _rotate_left(value, count):
    return ((value << count) | (value >> (32 - count))) & 0xffffffff


_MD5_SHIFTS = (
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
)

_MD5_CONSTANTS = (
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
)


def _md5(data):
    state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    message = bytearray(data)
    bit_length = len(message) * 8
    message.append(0x80)
    while len(message) % 64 != 56:
        message.append(0)
    message.extend(bit_length.to_bytes(8, "little"))
    for offset in range(0, len(message), 64):
        words = [int.from_bytes(message[offset + index:offset + index + 4], "little") for index in range(0, 64, 4)]
        a, b, c, d = state
        for index in range(64):
            if index < 16:
                function, word = (b & c) | (~b & d), index
            elif index < 32:
                function, word = (d & b) | (~d & c), (5 * index + 1) % 16
            elif index < 48:
                function, word = b ^ c ^ d, (3 * index + 5) % 16
            else:
                function, word = c ^ (b | ~d), (7 * index) % 16
            rotated = _rotate_left((a + function + _MD5_CONSTANTS[index] + words[word]) & 0xffffffff, _MD5_SHIFTS[index])
            a, d, c, b = d, c, b, (b + rotated) & 0xffffffff
        state = [(old + value) & 0xffffffff for old, value in zip(state, (a, b, c, d))]
    return b"".join(value.to_bytes(4, "little") for value in state)


def _sha1(data):
    state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]
    message = bytearray(data)
    bit_length = len(message) * 8
    message.append(0x80)
    while len(message) % 64 != 56:
        message.append(0)
    message.extend(bit_length.to_bytes(8, "big"))
    for offset in range(0, len(message), 64):
        words = [int.from_bytes(message[offset + index:offset + index + 4], "big") for index in range(0, 64, 4)]
        for index in range(16, 80):
            words.append(_rotate_left(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1))
        a, b, c, d, e = state
        for index in range(80):
            if index < 20:
                function, constant = (b & c) | (~b & d), 0x5a827999
            elif index < 40:
                function, constant = b ^ c ^ d, 0x6ed9eba1
            elif index < 60:
                function, constant = (b & c) | (b & d) | (c & d), 0x8f1bbcdc
            else:
                function, constant = b ^ c ^ d, 0xca62c1d6
            temporary = (_rotate_left(a, 5) + function + e + constant + words[index]) & 0xffffffff
            e, d, c, b, a = d, c, _rotate_left(b, 30), a, temporary
        state = [(old + value) & 0xffffffff for old, value in zip(state, (a, b, c, d, e))]
    return b"".join(value.to_bytes(4, "big") for value in state)


def _sha256(data, short=False):
    state = [
        0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4
    ] if short else [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    ]
    message = bytearray(data)
    bit_length = len(message) * 8
    message.append(0x80)
    while len(message) % 64 != 56:
        message.append(0)
    message.extend(bit_length.to_bytes(8, "big"))
    for offset in range(0, len(message), 64):
        words = [int.from_bytes(message[offset + index:offset + index + 4], "big") for index in range(0, 64, 4)]
        for index in range(16, 64):
            left = words[index - 15]
            right = words[index - 2]
            s0 = _rotate(left, 7) ^ _rotate(left, 18) ^ (left >> 3)
            s1 = _rotate(right, 17) ^ _rotate(right, 19) ^ (right >> 10)
            words.append((words[index - 16] + s0 + words[index - 7] + s1) & 0xffffffff)
        a, b, c, d, e, f, g, h = state
        for index in range(64):
            s1 = _rotate(e, 6) ^ _rotate(e, 11) ^ _rotate(e, 25)
            choose = (e & f) ^ (~e & g)
            first = (h + s1 + choose + _K256[index] + words[index]) & 0xffffffff
            s0 = _rotate(a, 2) ^ _rotate(a, 13) ^ _rotate(a, 22)
            majority = (a & b) ^ (a & c) ^ (b & c)
            second = (s0 + majority) & 0xffffffff
            h, g, f, e, d, c, b, a = g, f, e, (d + first) & 0xffffffff, c, b, a, (first + second) & 0xffffffff
        state = [(old + value) & 0xffffffff for old, value in zip(state, (a, b, c, d, e, f, g, h))]
    count = 7 if short else 8
    return b"".join(value.to_bytes(4, "big") for value in state[:count])


class HASH:
    def __init__(self, name, data=b""):
        self.name = name
        self._data = bytearray()
        self.update(data)
        self.digest_size = 16 if name == "md5" else 20 if name == "sha1" else 28 if name == "sha224" else 32
        self.block_size = 64

    def update(self, data):
        self._data.extend(data)

    def digest(self):
        if self.name == "md5":
            return _md5(self._data)
        if self.name == "sha1":
            return _sha1(self._data)
        return _sha256(self._data, self.name == "sha224")

    def hexdigest(self):
        return self.digest().hex()

    def copy(self):
        return HASH(self.name, self._data)


def openssl_sha224(data=b"", *, usedforsecurity=True):
    return HASH("sha224", data)


def openssl_sha1(data=b"", *, usedforsecurity=True):
    return HASH("sha1", data)


def openssl_md5(data=b"", *, usedforsecurity=True):
    return HASH("md5", data)


def openssl_sha256(data=b"", *, usedforsecurity=True):
    return HASH("sha256", data)


def new(name, data=b"", **kwargs):
    normalized = name.lower().replace("-", "")
    if normalized == "md5":
        return openssl_md5(data, **kwargs)
    if normalized == "sha1":
        return openssl_sha1(data, **kwargs)
    if normalized == "sha224":
        return openssl_sha224(data, **kwargs)
    if normalized == "sha256":
        return openssl_sha256(data, **kwargs)
    raise ValueError(f"unsupported hash type {name}")


def get_fips_mode():
    return 0
