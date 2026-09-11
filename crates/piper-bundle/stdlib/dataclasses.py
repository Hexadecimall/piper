class _Missing:
    def __repr__(self):
        return "<dataclasses.MISSING>"


MISSING = _Missing()
KW_ONLY = _Missing()


class Field:
    def __init__(self, default=MISSING, default_factory=MISSING, init=True, repr=True, hash=None, compare=True, metadata=None, kw_only=False, doc=None):
        self.name = None
        self.type = None
        self.default = default
        self.default_factory = default_factory
        self.init = init
        self.repr = repr
        self.hash = hash
        self.compare = compare
        self.metadata = {} if metadata is None else metadata
        self.kw_only = kw_only
        self.doc = doc

    @classmethod
    def __class_getitem__(cls, item):
        return cls


def field(*, default=MISSING, default_factory=MISSING, init=True, repr=True, hash=None, compare=True, metadata=None, kw_only=MISSING, doc=None):
    if default is not MISSING and default_factory is not MISSING:
        raise ValueError("cannot specify both default and default_factory")
    return Field(default, default_factory, init, repr, hash, compare, metadata, False if kw_only is MISSING else kw_only, doc)


def _decorate(cls, init=True, repr=True, eq=True, order=False, unsafe_hash=False, frozen=False, match_args=True, kw_only=False, slots=False, weakref_slot=False):
    annotations = getattr(cls, "__annotations__", {})
    declared = []
    for name in annotations:
        if name.startswith("__") or name == "_name_to_value":
            continue
        value = getattr(cls, name, MISSING)
        item = value if isinstance(value, Field) else Field(default=value, kw_only=kw_only)
        item.name = name
        item.type = annotations[name]
        declared.append(item)
    inherited = []
    for base in getattr(cls, "__mro__", ())[1:]:
        inherited.extend(getattr(base, "__dataclass_fields__", {}).values())
    all_fields = inherited + declared
    cls.__dataclass_fields__ = {item.name: item for item in all_fields}
    cls.__dataclass_params__ = (init, repr, eq, order, unsafe_hash, frozen)
    if match_args:
        cls.__match_args__ = tuple(item.name for item in all_fields if item.init and not item.kw_only)
    if init:
        def __init__(self, *args, **kwargs):
            positional = [item for item in all_fields if item.init and not item.kw_only]
            if len(args) > len(positional):
                raise TypeError(f"expected at most {len(positional)} positional arguments")
            supplied = set()
            for item, value in zip(positional, args):
                setattr(self, item.name, value)
                supplied.add(item.name)
            for item in all_fields:
                if item.name in supplied:
                    continue
                if item.name in kwargs:
                    setattr(self, item.name, kwargs.pop(item.name))
                elif item.default is not MISSING:
                    setattr(self, item.name, item.default)
                elif item.default_factory is not MISSING:
                    setattr(self, item.name, item.default_factory())
                elif item.init:
                    raise TypeError(f"missing required argument: {item.name!r}")
            if kwargs:
                name = next(iter(kwargs))
                raise TypeError(f"unexpected keyword argument {name!r}")
            post = getattr(self, "__post_init__", None)
            if post is not None:
                post()
        cls.__init__ = __init__
    if repr:
        def __repr__(self):
            values = ", ".join(f"{item.name}={getattr(self, item.name)!r}" for item in all_fields if item.repr)
            return f"{type(self).__qualname__}({values})"
        cls.__repr__ = __repr__
    if eq:
        def __eq__(self, other):
            if type(other) is not type(self):
                return NotImplemented
            return all(getattr(self, item.name) == getattr(other, item.name) for item in all_fields if item.compare)
        cls.__eq__ = __eq__
    return cls


def dataclass(cls=None, /, **options):
    def wrap(target):
        return _decorate(target, **options)
    return wrap if cls is None else wrap(cls)


def fields(class_or_instance):
    cls = class_or_instance if isinstance(class_or_instance, type) else type(class_or_instance)
    mapping = getattr(cls, "__dataclass_fields__", None)
    if mapping is None:
        raise TypeError("must be called with a dataclass type or instance")
    return tuple(mapping.values())


def is_dataclass(obj):
    return hasattr(obj if isinstance(obj, type) else type(obj), "__dataclass_fields__")


def asdict(obj, *, dict_factory=dict):
    if not is_dataclass(obj):
        raise TypeError("asdict() should be called on dataclass instances")
    return dict_factory((item.name, getattr(obj, item.name)) for item in fields(obj))


def astuple(obj, *, tuple_factory=tuple):
    if not is_dataclass(obj):
        raise TypeError("astuple() should be called on dataclass instances")
    return tuple_factory(getattr(obj, item.name) for item in fields(obj))


def replace(obj, **changes):
    if not is_dataclass(obj):
        raise TypeError("replace() should be called on dataclass instances")
    values = {item.name: changes.pop(item.name, getattr(obj, item.name)) for item in fields(obj) if item.init}
    if changes:
        raise TypeError(f"unexpected field name: {next(iter(changes))}")
    return type(obj)(**values)


def make_dataclass(cls_name, fields, *, bases=(), namespace=None, **options):
    namespace = {} if namespace is None else dict(namespace)
    annotations = {}
    for item in fields:
        if isinstance(item, str):
            annotations[item] = object
        elif len(item) == 2:
            annotations[item[0]] = item[1]
        else:
            annotations[item[0]] = item[1]
            namespace[item[0]] = item[2]
    namespace["__annotations__"] = annotations
    return dataclass(type(cls_name, bases or (object,), namespace), **options)
