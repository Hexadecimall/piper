use piper_syntax::dump::dump;
use piper_syntax::parse_module;

fn d(src: &str) -> String { dump(&parse_module(src, "<test>").unwrap()) }

#[test]
fn parses_a_name_expression_statement() {
    assert_eq!(d("x"), "Module(body=[Expr(value=Name(id='x', ctx=Load(), lineno=1, col_offset=0, end_lineno=1, end_col_offset=1), lineno=1, col_offset=0, end_lineno=1, end_col_offset=1)])");
}

#[test]
fn parses_binary_precedence() {
    assert_eq!(d("1 + 2 * 3"), "Module(body=[Expr(value=BinOp(left=Constant(value=1, lineno=1, col_offset=0, end_lineno=1, end_col_offset=1), op=Add(), right=BinOp(left=Constant(value=2, lineno=1, col_offset=4, end_lineno=1, end_col_offset=5), op=Mult(), right=Constant(value=3, lineno=1, col_offset=8, end_lineno=1, end_col_offset=9), lineno=1, col_offset=4, end_lineno=1, end_col_offset=9), lineno=1, col_offset=0, end_lineno=1, end_col_offset=9), lineno=1, col_offset=0, end_lineno=1, end_col_offset=9)])");
}

#[test]
fn parses_function_with_all_parameter_kinds() {
    assert_eq!(d("def f(a, /, b=1, *c, d, **e): pass"), "Module(body=[FunctionDef(name='f', args=arguments(posonlyargs=[arg(arg='a', lineno=1, col_offset=6, end_lineno=1, end_col_offset=7)], args=[arg(arg='b', lineno=1, col_offset=12, end_lineno=1, end_col_offset=13)], vararg=arg(arg='c', lineno=1, col_offset=18, end_lineno=1, end_col_offset=19), kwonlyargs=[arg(arg='d', lineno=1, col_offset=21, end_lineno=1, end_col_offset=22)], kw_defaults=[None], kwarg=arg(arg='e', lineno=1, col_offset=26, end_lineno=1, end_col_offset=27), defaults=[Constant(value=1, lineno=1, col_offset=14, end_lineno=1, end_col_offset=15)]), body=[Pass(lineno=1, col_offset=30, end_lineno=1, end_col_offset=34)], lineno=1, col_offset=0, end_lineno=1, end_col_offset=34)])");
}

#[test]
fn parses_fstring_with_conversion_and_nested_spec() {
    assert_eq!(d("f'a{b!r:>{w}}'"), "Module(body=[Expr(value=JoinedStr(values=[Constant(value='a', lineno=1, col_offset=2, end_lineno=1, end_col_offset=3), FormattedValue(value=Name(id='b', ctx=Load(), lineno=1, col_offset=4, end_lineno=1, end_col_offset=5), conversion=114, format_spec=JoinedStr(values=[Constant(value='>', lineno=1, col_offset=8, end_lineno=1, end_col_offset=9), FormattedValue(value=Name(id='w', ctx=Load(), lineno=1, col_offset=10, end_lineno=1, end_col_offset=11), conversion=-1, lineno=1, col_offset=9, end_lineno=1, end_col_offset=12)], lineno=1, col_offset=7, end_lineno=1, end_col_offset=12), lineno=1, col_offset=3, end_lineno=1, end_col_offset=13)], lineno=1, col_offset=0, end_lineno=1, end_col_offset=14), lineno=1, col_offset=0, end_lineno=1, end_col_offset=14)])");
}

#[test]
fn parses_match_with_or_sequence_mapping_and_class_patterns() {
    assert_eq!(d("match x:\n case [1, *r] | {'k': v, **rest} if v: pass\n case C(a, k=_): pass"), "Module(body=[Match(subject=Name(id='x', ctx=Load(), lineno=1, col_offset=6, end_lineno=1, end_col_offset=7), cases=[match_case(pattern=MatchOr(patterns=[MatchSequence(patterns=[MatchValue(value=Constant(value=1, lineno=2, col_offset=7, end_lineno=2, end_col_offset=8), lineno=2, col_offset=7, end_lineno=2, end_col_offset=8), MatchStar(name='r', lineno=2, col_offset=10, end_lineno=2, end_col_offset=12)], lineno=2, col_offset=6, end_lineno=2, end_col_offset=13), MatchMapping(keys=[Constant(value='k', lineno=2, col_offset=17, end_lineno=2, end_col_offset=20)], patterns=[MatchAs(name='v', lineno=2, col_offset=22, end_lineno=2, end_col_offset=23)], rest='rest', lineno=2, col_offset=16, end_lineno=2, end_col_offset=32)], lineno=2, col_offset=6, end_lineno=2, end_col_offset=32), guard=Name(id='v', ctx=Load(), lineno=2, col_offset=36, end_lineno=2, end_col_offset=37), body=[Pass(lineno=2, col_offset=39, end_lineno=2, end_col_offset=43)]), match_case(pattern=MatchClass(cls=Name(id='C', ctx=Load(), lineno=3, col_offset=6, end_lineno=3, end_col_offset=7), patterns=[MatchAs(name='a', lineno=3, col_offset=8, end_lineno=3, end_col_offset=9)], kwd_attrs=['k'], kwd_patterns=[MatchAs(lineno=3, col_offset=13, end_lineno=3, end_col_offset=14)], lineno=3, col_offset=6, end_lineno=3, end_col_offset=15), body=[Pass(lineno=3, col_offset=17, end_lineno=3, end_col_offset=21)])], lineno=1, col_offset=0, end_lineno=3, end_col_offset=21)])");
}

#[test]
fn parses_type_alias_with_all_type_param_kinds() {
    assert_eq!(d("type A[T: int = str, *Ts, **P] = list[T]"), "Module(body=[TypeAlias(name=Name(id='A', ctx=Store(), lineno=1, col_offset=5, end_lineno=1, end_col_offset=6), type_params=[TypeVar(name='T', bound=Name(id='int', ctx=Load(), lineno=1, col_offset=10, end_lineno=1, end_col_offset=13), default_value=Name(id='str', ctx=Load(), lineno=1, col_offset=16, end_lineno=1, end_col_offset=19), lineno=1, col_offset=7, end_lineno=1, end_col_offset=19), TypeVarTuple(name='Ts', lineno=1, col_offset=21, end_lineno=1, end_col_offset=24), ParamSpec(name='P', lineno=1, col_offset=26, end_lineno=1, end_col_offset=29)], value=Subscript(value=Name(id='list', ctx=Load(), lineno=1, col_offset=33, end_lineno=1, end_col_offset=37), slice=Name(id='T', ctx=Load(), lineno=1, col_offset=38, end_lineno=1, end_col_offset=39), ctx=Load(), lineno=1, col_offset=33, end_lineno=1, end_col_offset=40), lineno=1, col_offset=0, end_lineno=1, end_col_offset=40)])");
}

#[test]
fn implicit_concatenation_and_line_continuation_in_strings() {
    assert_eq!(d("x = 'a' 'b'"), "Module(body=[Assign(targets=[Name(id='x', ctx=Store(), lineno=1, col_offset=0, end_lineno=1, end_col_offset=1)], value=Constant(value='ab', lineno=1, col_offset=4, end_lineno=1, end_col_offset=11), lineno=1, col_offset=0, end_lineno=1, end_col_offset=11)])");
    assert_eq!(d("'''\\\nabc'''"), "Module(body=[Expr(value=Constant(value='abc', lineno=1, col_offset=0, end_lineno=2, end_col_offset=6), lineno=1, col_offset=0, end_lineno=2, end_col_offset=6)])");
}

#[test]
fn reports_syntax_errors_with_position() {
    let err = parse_module("x = (1,\n", "<test>").unwrap_err();
    assert_eq!(err.lineno, 1);
    let err = parse_module("def f(:\n  pass\n", "<test>").unwrap_err();
    assert_eq!(err.lineno, 1);
    assert!(err.msg.contains("invalid syntax"), "{}", err.msg);
}
