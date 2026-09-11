from _opcode_metadata import opmap


ENABLE_SPECIALIZATION = True
ENABLE_SPECIALIZATION_FT = True


def _name(opcode):
    for name, value in opmap.items():
        if value == opcode:
            return name
    return ""


def is_valid(opcode):
    return isinstance(opcode, int) and 0 <= opcode <= 255


def has_arg(opcode):
    return _name(opcode) not in ("", "CACHE", "POP_TOP", "PUSH_NULL", "NOP", "UNARY_NEGATIVE", "UNARY_NOT", "UNARY_INVERT", "GET_ITER", "RETURN_VALUE", "RESUME_CHECK", "TO_BOOL")


def has_const(opcode):
    return _name(opcode) in ("LOAD_CONST", "RETURN_CONST")


def has_name(opcode):
    return _name(opcode) in ("STORE_NAME", "DELETE_NAME", "STORE_ATTR", "DELETE_ATTR", "STORE_GLOBAL", "DELETE_GLOBAL", "LOAD_NAME", "LOAD_ATTR", "LOAD_GLOBAL", "IMPORT_NAME", "IMPORT_FROM", "LOAD_SUPER_ATTR")


def has_jump(opcode):
    name = _name(opcode)
    return "JUMP" in name or name in ("FOR_ITER", "SEND")


def has_free(opcode):
    return _name(opcode) in ("LOAD_FROM_DICT_OR_DEREF", "LOAD_DEREF", "STORE_DEREF", "DELETE_DEREF", "LOAD_CLOSURE")


def has_local(opcode):
    return _name(opcode) in ("LOAD_FAST", "STORE_FAST", "DELETE_FAST", "LOAD_FAST_CHECK", "LOAD_FAST_AND_CLEAR")


def has_exc(opcode):
    return _name(opcode) in ("SETUP_WITH", "SETUP_CLEANUP", "SETUP_FINALLY")


def stack_effect(opcode, oparg=None, *, jump=None):
    name = _name(opcode)
    if name.startswith("LOAD_") or name in ("PUSH_NULL", "COPY"):
        return 1
    if name.startswith("STORE_") or name in ("POP_TOP", "RETURN_VALUE", "RAISE_VARARGS"):
        return -1
    if name in ("BINARY_OP", "COMPARE_OP", "CONTAINS_OP", "IS_OP"):
        return -1
    if name in ("BUILD_TUPLE", "BUILD_LIST", "BUILD_SET", "BUILD_STRING"):
        return 1 - (oparg or 0)
    if name == "BUILD_MAP":
        return 1 - 2 * (oparg or 0)
    if name in ("UNARY_NEGATIVE", "UNARY_NOT", "UNARY_INVERT", "NOP", "RESUME", "RESUME_CHECK"):
        return 0
    return 0


def get_intrinsic1_descs():
    return ["INTRINSIC_1_INVALID", "INTRINSIC_PRINT", "INTRINSIC_IMPORT_STAR", "INTRINSIC_STOPITERATION_ERROR", "INTRINSIC_ASYNC_GEN_WRAP", "INTRINSIC_UNARY_POSITIVE", "INTRINSIC_LIST_TO_TUPLE", "INTRINSIC_TYPEVAR", "INTRINSIC_PARAMSPEC", "INTRINSIC_TYPEVARTUPLE", "INTRINSIC_SUBSCRIPT_GENERIC", "INTRINSIC_TYPEALIAS"]


def get_intrinsic2_descs():
    return ["INTRINSIC_2_INVALID", "INTRINSIC_PREP_RERAISE_STAR", "INTRINSIC_TYPEVAR_WITH_BOUND", "INTRINSIC_TYPEVAR_WITH_CONSTRAINTS", "INTRINSIC_SET_FUNCTION_TYPE_PARAMS", "INTRINSIC_SET_TYPEPARAM_DEFAULT"]


def get_special_method_names():
    return ["__enter__", "__exit__", "__aenter__", "__aexit__"]


def get_nb_ops():
    symbols = ("+", "&", "//", "<<", "@", "*", "%", "|", "**", ">>", "-", "/", "^")
    names = ("ADD", "AND", "FLOOR_DIVIDE", "LSHIFT", "MATRIX_MULTIPLY", "MULTIPLY", "REMAINDER", "OR", "POWER", "RSHIFT", "SUBTRACT", "TRUE_DIVIDE", "XOR")
    result = [("NB_" + name, symbol) for name, symbol in zip(names, symbols)]
    result.extend(("NB_INPLACE_" + name, symbol + "=") for name, symbol in zip(names, symbols))
    result.append(("NB_SUBSCR", "[]"))
    return result


def get_specialization_stats():
    return None


def get_executor(code, offset):
    raise ValueError("no executor at the specified offset")
