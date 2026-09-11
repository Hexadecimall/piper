SOURCE_SUFFIXES = [".py"]
BYTECODE_SUFFIXES = [".pyc"]
DEBUG_BYTECODE_SUFFIXES = BYTECODE_SUFFIXES
OPTIMIZED_BYTECODE_SUFFIXES = BYTECODE_SUFFIXES
EXTENSION_SUFFIXES = [".so", ".pyd", ".dylib"]


def all_suffixes():
    return SOURCE_SUFFIXES + BYTECODE_SUFFIXES + EXTENSION_SUFFIXES


class ModuleSpec:
    def __init__(self, name, loader, *, origin=None, loader_state=None, is_package=None):
        self.name = name
        self.loader = loader
        self.origin = origin
        self.loader_state = loader_state
        self.submodule_search_locations = [] if is_package else None
        self.cached = None
        self.has_location = origin is not None


class BuiltinImporter:
    pass


class FrozenImporter:
    pass


class PathFinder:
    pass


class FileFinder:
    pass


class SourceFileLoader:
    pass


class SourcelessFileLoader:
    pass


class ExtensionFileLoader:
    pass


class AppleFrameworkLoader(ExtensionFileLoader):
    pass


class NamespaceLoader:
    pass


class WindowsRegistryFinder:
    pass
