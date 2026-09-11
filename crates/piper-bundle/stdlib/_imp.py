pyc_magic_number_token = 3571
check_hash_based_pycs = "default"


def extension_suffixes():
    return [".so", ".pyd", ".dylib"]


def acquire_lock():
    return None


def release_lock():
    return None


def lock_held():
    return False


def is_builtin(name):
    return 0


def is_frozen(name):
    return False


def is_frozen_package(name):
    raise ImportError(f"No such frozen object named {name!r}")


def find_frozen(name, withdata=False):
    return None


def _frozen_module_names():
    return ()


def get_frozen_object(name, data=None):
    raise ImportError(f"No such frozen object named {name!r}")


def init_frozen(name):
    return None


def create_builtin(spec):
    return None


def exec_builtin(module):
    return 0


def create_dynamic(spec, file=None):
    raise ImportError(f"dynamic module {spec.name!r} must be loaded by Piper's import system")


def exec_dynamic(module):
    return 0


def source_hash(key, source):
    value = key & 0xffffffffffffffff
    for byte in source:
        value ^= byte
        value = value * 1099511628211 & 0xffffffffffffffff
    return value.to_bytes(8, "little")


def _fix_co_filename(code, path):
    return None


def _override_multi_interp_extensions_check(override):
    return None


def _override_frozen_modules_for_tests(override):
    return None
