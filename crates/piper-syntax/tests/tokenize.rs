use piper_syntax::token::{Tok, tokenize};

fn kinds(src: &str) -> Vec<Tok> {
    tokenize(src).unwrap().into_iter().map(|t| t.tok).collect()
}
fn name(s: &str) -> Tok { Tok::Name(s.into()) }
fn op(s: &str) -> Tok { Tok::Op(s.into()) }
fn num(s: &str) -> Tok { Tok::Number(s.into()) }
fn string(s: &str) -> Tok { Tok::String(s.into()) }

#[test]
fn tokenizes_a_name_and_an_integer() {
    assert_eq!(kinds("x = 42\n"), vec![name("x"), op("="), num("42"), Tok::Newline, Tok::EndMarker]);
}

#[test]
fn adds_missing_final_newline() {
    assert_eq!(kinds("x"), vec![name("x"), Tok::Newline, Tok::EndMarker]);
}

#[test]
fn emits_indent_and_dedent_for_blocks() {
    assert_eq!(
        kinds("if a:\n    b\nc\n"),
        vec![
            name("if"), name("a"), op(":"), Tok::Newline,
            Tok::Indent, name("b"), Tok::Newline,
            Tok::Dedent, name("c"), Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn dedents_all_open_blocks_at_end() {
    assert_eq!(
        kinds("if a:\n  if b:\n    c\n"),
        vec![
            name("if"), name("a"), op(":"), Tok::Newline,
            Tok::Indent, name("if"), name("b"), op(":"), Tok::Newline,
            Tok::Indent, name("c"), Tok::Newline,
            Tok::Dedent, Tok::Dedent, Tok::EndMarker,
        ]
    );
}

#[test]
fn blank_and_comment_lines_are_nl_not_newline() {
    assert_eq!(
        kinds("a\n\n# hi\nb\n"),
        vec![
            name("a"), Tok::Newline, Tok::Nl,
            Tok::Comment("# hi".into()), Tok::Nl,
            name("b"), Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn trailing_comment_precedes_newline() {
    assert_eq!(
        kinds("a  # c\n"),
        vec![name("a"), Tok::Comment("# c".into()), Tok::Newline, Tok::EndMarker]
    );
}

#[test]
fn longest_operator_wins() {
    assert_eq!(
        kinds("a **= b >>= c // d -> e := f ... g != h\n"),
        vec![
            name("a"), op("**="), name("b"), op(">>="), name("c"), op("//"), name("d"),
            op("->"), name("e"), op(":="), name("f"), op("..."), name("g"), op("!="),
            name("h"), Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn newlines_inside_brackets_are_nl_and_do_not_indent() {
    assert_eq!(
        kinds("f(a,\n    b)\n"),
        vec![
            name("f"), op("("), name("a"), op(","), Tok::Nl,
            name("b"), op(")"), Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn backslash_continues_a_line() {
    assert_eq!(
        kinds("a + \\\n  b\n"),
        vec![name("a"), op("+"), name("b"), Tok::Newline, Tok::EndMarker]
    );
}

#[test]
fn number_forms() {
    assert_eq!(
        kinds("0x_FF 0o17 0b1_0 1_000 3.14 .5 1e10 1.5E-3 2j 0 00 1.j\n"),
        vec![
            num("0x_FF"), num("0o17"), num("0b1_0"), num("1_000"), num("3.14"), num(".5"),
            num("1e10"), num("1.5E-3"), num("2j"), num("0"), num("00"), num("1.j"),
            Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn string_forms_keep_prefix_and_quotes() {
    assert_eq!(
        kinds(r#"'a' "b" r'\d' b"x" Rb'y' u"z" '''m\nl''' "" 'it\'s'"#),
        vec![
            string("'a'"), string("\"b\""), string(r"r'\d'"), string("b\"x\""), string("Rb'y'"),
            string("u\"z\""), string("'''m\\nl'''"), string("\"\""), string(r"'it\'s'"),
            Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn triple_quoted_string_spans_lines() {
    let toks = tokenize("s = '''a\nb'''\nc\n").unwrap();
    assert_eq!(toks[2].tok, string("'''a\nb'''"));
    assert_eq!((toks[2].start, toks[2].end), ((1, 4), (2, 4)));
    assert_eq!(toks[4].tok, name("c"));
}

#[test]
fn fstring_splits_into_start_middle_end() {
    assert_eq!(
        kinds("f'a{b!r:>{w}} c'\n"),
        vec![
            Tok::FStringStart("f'".into()), Tok::FStringMiddle("a".into()),
            op("{"), name("b"), op("!"), name("r"), op(":"), Tok::FStringMiddle(">".into()),
            op("{"), name("w"), op("}"), Tok::FStringMiddle("".into()), op("}"),
            Tok::FStringMiddle(" c".into()), Tok::FStringEnd("'".into()),
            Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn fstring_nests_same_quotes_pep701() {
    assert_eq!(
        kinds("f'{x['k']}'\n"),
        vec![
            Tok::FStringStart("f'".into()), op("{"), name("x"), op("["), string("'k'"),
            op("]"), op("}"), Tok::FStringEnd("'".into()), Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn fstring_doubled_braces_are_literal_middle() {
    assert_eq!(
        kinds("f'{{a}}{b}'\n"),
        vec![
            Tok::FStringStart("f'".into()), Tok::FStringMiddle("{".into()),
            Tok::FStringMiddle("a}".into()),
            op("{"), name("b"), op("}"), Tok::FStringEnd("'".into()),
            Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn tstring_pep750_tokenizes_like_fstring() {
    assert_eq!(
        kinds("t'hi {name}'\n"),
        vec![
            Tok::TStringStart("t'".into()), Tok::TStringMiddle("hi ".into()),
            op("{"), name("name"), op("}"), Tok::TStringEnd("'".into()),
            Tok::Newline, Tok::EndMarker,
        ]
    );
}

#[test]
fn positions_are_one_based_lines_and_zero_based_columns() {
    let toks = tokenize("ab = 1\n").unwrap();
    assert_eq!((toks[0].start, toks[0].end), ((1, 0), (1, 2)));
    assert_eq!((toks[1].start, toks[1].end), ((1, 3), (1, 4)));
    assert_eq!((toks[3].start, toks[3].end), ((1, 6), (1, 7)));
}

#[test]
fn non_ascii_identifiers_are_names() {
    assert_eq!(kinds("héllo = ñ\n"), vec![name("héllo"), op("="), name("ñ"), Tok::Newline, Tok::EndMarker]);
}

#[test]
fn inconsistent_dedent_is_an_error() {
    let err = tokenize("if a:\n    b\n  c\n").unwrap_err();
    assert_eq!(err.line, 3);
    assert!(err.msg.contains("unindent"), "{}", err.msg);
}

#[test]
fn unterminated_string_is_an_error() {
    let err = tokenize("x = 'abc\n").unwrap_err();
    assert_eq!(err.line, 1);
    assert!(err.msg.contains("unterminated"), "{}", err.msg);
}

#[test]
fn stray_character_is_an_error() {
    let err = tokenize("a $ b\n").unwrap_err();
    assert_eq!((err.line, err.col), (1, 2));
}

#[test]
fn byte_columns_count_utf8_bytes() {
    let toks = tokenize("é = 1\n").unwrap();
    assert_eq!((toks[0].start, toks[0].end), ((1, 0), (1, 1)));
    assert_eq!((toks[0].start_byte, toks[0].end_byte), (0, 2));
    assert_eq!((toks[1].start_byte, toks[1].end_byte), (3, 4));
}
