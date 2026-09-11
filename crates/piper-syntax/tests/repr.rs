use piper_syntax::dump::{py_bytes_repr, py_float_repr, py_str_repr};

#[test]
fn float_repr_matches_python() {
    for (f, want) in [
        (1.0, "1.0"), (1.5, "1.5"), (1e16, "1e+16"), (1e15, "1000000000000000.0"), (1e-5, "1e-05"),
        (0.0001, "0.0001"), (1e22, "1e+22"), (3.0, "3.0"), (0.1, "0.1"), (123456.789, "123456.789"),
        (1.7976931348623157e308, "1.7976931348623157e+308"), (5e-324, "5e-324"), (0.0, "0.0"), (-0.0, "-0.0"),
        (2.5e-7, "2.5e-07"), (f64::INFINITY, "inf"), (f64::NAN, "nan"), (100.0, "100.0"), (1234567890123456.0, "1234567890123456.0"),
    ] {
        assert_eq!(py_float_repr(f), want, "{f:?}");
    }
}

#[test]
fn str_repr_matches_python() {
    assert_eq!(py_str_repr("a"), "'a'");
    assert_eq!(py_str_repr("it's"), "\"it's\"");
    assert_eq!(py_str_repr("\""), "'\"'");
    assert_eq!(py_str_repr("'\""), "'\\'\"'");
    assert_eq!(py_str_repr("é\n\t\x7f\x01"), "'é\\n\\t\\x7f\\x01'");
    assert_eq!(py_str_repr("\u{200b}\u{1F600}\u{e0001}"), "'\\u200b😀\\U000e0001'");
    assert_eq!(py_str_repr("\\"), "'\\\\'");
}

#[test]
fn bytes_repr_matches_python() {
    assert_eq!(py_bytes_repr(b"\x00a'\n"), "b\"\\x00a'\\n\"");
    assert_eq!(py_bytes_repr(b"\xff\\"), "b'\\xff\\\\'");
}
