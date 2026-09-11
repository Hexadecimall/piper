PyCF_ONLY_AST = 1024
PyCF_TYPE_COMMENTS = 4096
PyCF_ALLOW_TOP_LEVEL_AWAIT = 8192
PyCF_OPTIMIZED_AST = 32768


class AST:
    _fields = ()
    _attributes = ()
    _field_types = {}

    def __init__(self, *args, **kwargs):
        if len(args) > len(self._fields):
            raise TypeError(f"{type(self).__name__} constructor takes at most {len(self._fields)} positional arguments")
        for name, value in zip(self._fields, args):
            setattr(self, name, value)
        for name, value in kwargs.items():
            setattr(self, name, value)


def _make(name, base, fields=(), attributes=None):
    if attributes is None:
        attributes = base._attributes
    return type(name, (base,), {
        "_fields": fields,
        "_attributes": attributes,
        "_field_types": {},
        "__module__": "ast",
    })


mod = _make("mod", AST)
stmt = _make("stmt", AST, attributes=("lineno", "col_offset", "end_lineno", "end_col_offset"))
expr = _make("expr", AST, attributes=("lineno", "col_offset", "end_lineno", "end_col_offset"))
expr_context = _make("expr_context", AST)
boolop = _make("boolop", AST)
operator = _make("operator", AST)
unaryop = _make("unaryop", AST)
cmpop = _make("cmpop", AST)
comprehension = _make("comprehension", AST, ("target", "iter", "ifs", "is_async"))
excepthandler = _make("excepthandler", AST, attributes=stmt._attributes)
pattern = _make("pattern", AST, attributes=stmt._attributes)
type_ignore = _make("type_ignore", AST)
type_param = _make("type_param", AST, attributes=stmt._attributes)
arguments = _make("arguments", AST, ("posonlyargs", "args", "vararg", "kwonlyargs", "kw_defaults", "kwarg", "defaults"))
arg = _make("arg", AST, ("arg", "annotation", "type_comment"), stmt._attributes)
keyword = _make("keyword", AST, ("arg", "value"), stmt._attributes)
alias = _make("alias", AST, ("name", "asname"), stmt._attributes)
withitem = _make("withitem", AST, ("context_expr", "optional_vars"))
match_case = _make("match_case", AST, ("pattern", "guard", "body"))


for _name in ("Load", "Store", "Del"):
    globals()[_name] = _make(_name, expr_context)
for _name in ("And", "Or"):
    globals()[_name] = _make(_name, boolop)
for _name in ("Add", "Sub", "Mult", "MatMult", "Div", "Mod", "Pow", "LShift", "RShift", "BitOr", "BitXor", "BitAnd", "FloorDiv"):
    globals()[_name] = _make(_name, operator)
for _name in ("Invert", "Not", "UAdd", "USub"):
    globals()[_name] = _make(_name, unaryop)
for _name in ("Eq", "NotEq", "Lt", "LtE", "Gt", "GtE", "Is", "IsNot", "In", "NotIn"):
    globals()[_name] = _make(_name, cmpop)


_nodes = (
    ("Module", mod, ("body", "type_ignores")), ("Interactive", mod, ("body",)),
    ("Expression", mod, ("body",)), ("FunctionType", mod, ("argtypes", "returns")),
    ("FunctionDef", stmt, ("name", "args", "body", "decorator_list", "returns", "type_comment", "type_params")),
    ("AsyncFunctionDef", stmt, ("name", "args", "body", "decorator_list", "returns", "type_comment", "type_params")),
    ("ClassDef", stmt, ("name", "bases", "keywords", "body", "decorator_list", "type_params")),
    ("Return", stmt, ("value",)), ("Delete", stmt, ("targets",)),
    ("Assign", stmt, ("targets", "value", "type_comment")),
    ("TypeAlias", stmt, ("name", "type_params", "value")),
    ("AugAssign", stmt, ("target", "op", "value")),
    ("AnnAssign", stmt, ("target", "annotation", "value", "simple")),
    ("For", stmt, ("target", "iter", "body", "orelse", "type_comment")),
    ("AsyncFor", stmt, ("target", "iter", "body", "orelse", "type_comment")),
    ("While", stmt, ("test", "body", "orelse")), ("If", stmt, ("test", "body", "orelse")),
    ("With", stmt, ("items", "body", "type_comment")),
    ("AsyncWith", stmt, ("items", "body", "type_comment")),
    ("Match", stmt, ("subject", "cases")), ("Raise", stmt, ("exc", "cause")),
    ("Try", stmt, ("body", "handlers", "orelse", "finalbody")),
    ("TryStar", stmt, ("body", "handlers", "orelse", "finalbody")),
    ("Assert", stmt, ("test", "msg")), ("Import", stmt, ("names",)),
    ("ImportFrom", stmt, ("module", "names", "level")), ("Global", stmt, ("names",)),
    ("Nonlocal", stmt, ("names",)), ("Expr", stmt, ("value",)),
    ("Pass", stmt, ()), ("Break", stmt, ()), ("Continue", stmt, ()),
    ("BoolOp", expr, ("op", "values")), ("NamedExpr", expr, ("target", "value")),
    ("BinOp", expr, ("left", "op", "right")), ("UnaryOp", expr, ("op", "operand")),
    ("Lambda", expr, ("args", "body")), ("IfExp", expr, ("test", "body", "orelse")),
    ("Dict", expr, ("keys", "values")), ("Set", expr, ("elts",)),
    ("ListComp", expr, ("elt", "generators")), ("SetComp", expr, ("elt", "generators")),
    ("DictComp", expr, ("key", "value", "generators")), ("GeneratorExp", expr, ("elt", "generators")),
    ("Await", expr, ("value",)), ("Yield", expr, ("value",)), ("YieldFrom", expr, ("value",)),
    ("Compare", expr, ("left", "ops", "comparators")), ("Call", expr, ("func", "args", "keywords")),
    ("FormattedValue", expr, ("value", "conversion", "format_spec")),
    ("JoinedStr", expr, ("values",)), ("Interpolation", expr, ("value", "str", "conversion", "format_spec")),
    ("TemplateStr", expr, ("values",)), ("Constant", expr, ("value", "kind")),
    ("Attribute", expr, ("value", "attr", "ctx")), ("Subscript", expr, ("value", "slice", "ctx")),
    ("Starred", expr, ("value", "ctx")), ("Name", expr, ("id", "ctx")),
    ("List", expr, ("elts", "ctx")), ("Tuple", expr, ("elts", "ctx")),
    ("Slice", expr, ("lower", "upper", "step")),
    ("ExceptHandler", excepthandler, ("type", "name", "body")),
    ("MatchValue", pattern, ("value",)), ("MatchSingleton", pattern, ("value",)),
    ("MatchSequence", pattern, ("patterns",)), ("MatchMapping", pattern, ("keys", "patterns", "rest")),
    ("MatchClass", pattern, ("cls", "patterns", "kwd_attrs", "kwd_patterns")),
    ("MatchStar", pattern, ("name",)), ("MatchAs", pattern, ("pattern", "name")),
    ("MatchOr", pattern, ("patterns",)), ("TypeIgnore", type_ignore, ("lineno", "tag")),
    ("TypeVar", type_param, ("name", "bound", "default_value")),
    ("ParamSpec", type_param, ("name", "default_value")),
    ("TypeVarTuple", type_param, ("name", "default_value")),
)

for _name, _base, _fields in _nodes:
    globals()[_name] = _make(_name, _base, _fields)
