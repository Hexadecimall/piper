import _hashlib


algorithms_guaranteed = {"md5", "sha1", "sha224", "sha256"}
algorithms_available = set(_hashlib.algorithms_available)

md5 = _hashlib.openssl_md5
sha1 = _hashlib.openssl_sha1
sha224 = _hashlib.openssl_sha224
sha256 = _hashlib.openssl_sha256
new = _hashlib.new


def file_digest(fileobj, digest, /):
    constructor = new if isinstance(digest, str) else digest
    result = constructor(digest) if constructor is new else constructor()
    buffer = bytearray(262144)
    view = memoryview(buffer)
    while True:
        if hasattr(fileobj, "readinto"):
            size = fileobj.readinto(buffer)
            if not size:
                break
            result.update(view[:size])
        else:
            data = fileobj.read(len(buffer))
            if not data:
                break
            result.update(data)
    return result
