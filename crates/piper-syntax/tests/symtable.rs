use piper_syntax::parse_module;
use piper_syntax::symtable::{Scope, ScopeKind, SymbolTable, analyze};

fn table(src: &str) -> SymbolTable { analyze(&parse_module(src, "<t>").unwrap()).unwrap() }
fn child<'a>(t: &'a SymbolTable, parent: &Scope, i: usize) -> &'a Scope { &t.scopes[parent.children[i]] }

#[test]
fn module_names_are_global() {
    let t = table("x = 1\nprint(x)\n");
    let m = t.module();
    assert_eq!(m.kind, ScopeKind::Module);
    assert!(m.is_global(&"x".to_string()));
    assert!(m.is_global(&"print".to_string()));
}

#[test]
fn function_locals_params_and_implicit_globals() {
    let t = table("def f(a, b=1, *args, c, **kw):\n    x = a\n    return x + y\n");
    let f = child(&t, t.module(), 0);
    assert_eq!(f.kind, ScopeKind::Function);
    assert_eq!(f.name, "f");
    assert_eq!(f.params, vec!["a", "b", "c", "args", "kw"]);
    assert!(f.is_local("x") && f.is_local("a"));
    assert!(f.is_global_implicit("y"));
}

#[test]
fn closures_produce_cells_and_frees() {
    let t = table("def outer():\n    v = 1\n    w = 2\n    def inner():\n        return v\n    return inner, w\n");
    let outer = child(&t, t.module(), 0);
    let inner = child(&t, outer, 0);
    assert_eq!(outer.cellvars, vec!["v"]);
    assert!(outer.is_local("w") && !outer.cellvars.contains(&"w".to_string()));
    assert_eq!(inner.freevars, vec!["v"]);
}

#[test]
fn free_variables_pass_through_intermediate_scopes() {
    let t = table("def a():\n    x = 0\n    def b():\n        def c():\n            return x\n        return c\n    return b\n");
    let a = child(&t, t.module(), 0);
    let b = child(&t, a, 0);
    let c = child(&t, b, 0);
    assert_eq!(a.cellvars, vec!["x"]);
    assert_eq!(b.freevars, vec!["x"]);
    assert_eq!(c.freevars, vec!["x"]);
}

#[test]
fn global_and_nonlocal_declarations() {
    let t = table("g = 0\ndef f():\n    global g\n    g = 1\n    n = 0\n    def h():\n        nonlocal n\n        n += 1\n    return h\n");
    let f = child(&t, t.module(), 0);
    let h = child(&t, f, 0);
    assert!(f.is_global_explicit("g"));
    assert_eq!(f.cellvars, vec!["n"]);
    assert_eq!(h.freevars, vec!["n"]);
}

#[test]
fn class_scope_does_not_leak_to_methods_and_has_class_cell_for_super() {
    let t = table("def f():\n    y = 1\n    class C:\n        x = 1\n        def m(self):\n            return x + y + super().m()\n    return C\n");
    let f = child(&t, t.module(), 0);
    let c = child(&t, f, 0);
    let m = child(&t, c, 0);
    assert_eq!(c.kind, ScopeKind::Class);
    assert!(c.is_local("x"));
    assert!(m.is_global_implicit("x"), "class-level x is not visible in methods");
    assert!(m.freevars.contains(&"y".to_string()));
    assert!(m.freevars.contains(&"__class__".to_string()));
    assert!(c.needs_class_cell);
}

#[test]
fn class_scope_passes_outer_cells_to_methods() {
    let t = table("def outer(value):\n    class Inner:\n        value = 1\n        def read(self):\n            return value\n    return Inner\n");
    let outer = &t.scopes[t.scopes[0].children[0]];
    let class = &t.scopes[outer.children[0]];
    let method = &t.scopes[class.children[0]];
    assert!(outer.cellvars.contains(&"value".to_string()));
    assert!(class.freevars.contains(&"value".to_string()));
    assert!(method.freevars.contains(&"value".to_string()));
    assert!(class.is_local("value"));
}

#[test]
fn comprehensions_are_child_scopes_and_walrus_binds_outside() {
    let t = table("def f(xs):\n    r = [y for y in xs if (z := y)]\n    return r, z\n");
    let f = child(&t, t.module(), 0);
    let comp = child(&t, f, 0);
    assert_eq!(comp.kind, ScopeKind::Comprehension);
    assert!(comp.is_local("y"));
    assert!(f.is_local("z"));
    assert!(comp.freevars.contains(&"z".to_string()) || comp.is_local("z") == false);
    assert_eq!(comp.params, vec![".0"]);
}

#[test]
fn generators_and_coroutines_are_flagged() {
    let t = table("def g():\n    yield 1\nasync def c():\n    await x\nlam = lambda: (yield)\n");
    let g = child(&t, t.module(), 0);
    let c = child(&t, t.module(), 1);
    let l = child(&t, t.module(), 2);
    assert!(g.is_generator && !g.is_coroutine);
    assert!(c.is_coroutine);
    assert!(l.is_generator);
    assert_eq!(l.name, "<lambda>");
}

#[test]
fn duplicate_or_conflicting_declarations_are_errors() {
    assert!(analyze(&parse_module("def f(a, a): pass", "<t>").unwrap()).is_err());
    assert!(analyze(&parse_module("def f():\n    x = 1\n    global x\n", "<t>").unwrap()).is_err());
    assert!(analyze(&parse_module("nonlocal q\n", "<t>").unwrap()).is_err());
}
