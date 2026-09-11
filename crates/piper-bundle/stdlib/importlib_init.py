def import_module(name, package=None):
    if name.startswith("."):
        if package is None:
            raise TypeError("the 'package' argument is required for relative imports")
        level = len(name) - len(name.lstrip("."))
        parts = package.split(".")
        if level > len(parts):
            raise ImportError("attempted relative import beyond top-level package")
        prefix = ".".join(parts[:len(parts) - level + 1])
        tail = name[level:]
        name = prefix + ("." if prefix and tail else "") + tail
    return __import__(name, fromlist=["*"])


def invalidate_caches():
    return None


def reload(module):
    name = getattr(module, "__name__", None)
    if not isinstance(name, str):
        raise TypeError("reload() argument must be a module")
    return import_module(name)
