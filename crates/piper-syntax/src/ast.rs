//! Abstract syntax tree. Node set and field names follow CPython 3.14's
//! `Python.asdl` one to one so the `ast` module can serve as an oracle.

/// Source span. Lines are 1-based; columns are 0-based UTF-8 byte offsets.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Span {
    pub lineno: u32,
    pub col_offset: u32,
    pub end_lineno: u32,
    pub end_col_offset: u32,
}

impl Span {
    pub fn new(lineno: u32, col_offset: u32, end_lineno: u32, end_col_offset: u32) -> Self {
        Span { lineno, col_offset, end_lineno, end_col_offset }
    }
    /// Span from the start of `self` to the end of `other`.
    pub fn to(self, other: Span) -> Span {
        Span { lineno: self.lineno, col_offset: self.col_offset, end_lineno: other.end_lineno, end_col_offset: other.end_col_offset }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Mod {
    Module { body: Vec<Stmt>, type_ignores: Vec<TypeIgnore> },
    Interactive { body: Vec<Stmt> },
    Expression { body: Box<Expr> },
    FunctionType { argtypes: Vec<Expr>, returns: Box<Expr> },
}

#[derive(Debug, Clone, PartialEq)]
pub struct TypeIgnore {
    pub lineno: u32,
    pub tag: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub enum StmtKind {
    FunctionDef {
        name: String,
        args: Box<Arguments>,
        body: Vec<Stmt>,
        decorator_list: Vec<Expr>,
        returns: Option<Box<Expr>>,
        type_comment: Option<String>,
        type_params: Vec<TypeParam>,
    },
    AsyncFunctionDef {
        name: String,
        args: Box<Arguments>,
        body: Vec<Stmt>,
        decorator_list: Vec<Expr>,
        returns: Option<Box<Expr>>,
        type_comment: Option<String>,
        type_params: Vec<TypeParam>,
    },
    ClassDef {
        name: String,
        bases: Vec<Expr>,
        keywords: Vec<Keyword>,
        body: Vec<Stmt>,
        decorator_list: Vec<Expr>,
        type_params: Vec<TypeParam>,
    },
    Return { value: Option<Box<Expr>> },
    Delete { targets: Vec<Expr> },
    Assign { targets: Vec<Expr>, value: Box<Expr>, type_comment: Option<String> },
    TypeAlias { name: Box<Expr>, type_params: Vec<TypeParam>, value: Box<Expr> },
    AugAssign { target: Box<Expr>, op: Operator, value: Box<Expr> },
    AnnAssign { target: Box<Expr>, annotation: Box<Expr>, value: Option<Box<Expr>>, simple: bool },
    For { target: Box<Expr>, iter: Box<Expr>, body: Vec<Stmt>, orelse: Vec<Stmt>, type_comment: Option<String> },
    AsyncFor { target: Box<Expr>, iter: Box<Expr>, body: Vec<Stmt>, orelse: Vec<Stmt>, type_comment: Option<String> },
    While { test: Box<Expr>, body: Vec<Stmt>, orelse: Vec<Stmt> },
    If { test: Box<Expr>, body: Vec<Stmt>, orelse: Vec<Stmt> },
    With { items: Vec<WithItem>, body: Vec<Stmt>, type_comment: Option<String> },
    AsyncWith { items: Vec<WithItem>, body: Vec<Stmt>, type_comment: Option<String> },
    Match { subject: Box<Expr>, cases: Vec<MatchCase> },
    Raise { exc: Option<Box<Expr>>, cause: Option<Box<Expr>> },
    Try { body: Vec<Stmt>, handlers: Vec<ExceptHandler>, orelse: Vec<Stmt>, finalbody: Vec<Stmt> },
    TryStar { body: Vec<Stmt>, handlers: Vec<ExceptHandler>, orelse: Vec<Stmt>, finalbody: Vec<Stmt> },
    Assert { test: Box<Expr>, msg: Option<Box<Expr>> },
    Import { names: Vec<Alias> },
    ImportFrom { module: Option<String>, names: Vec<Alias>, level: u32 },
    Global { names: Vec<String> },
    Nonlocal { names: Vec<String> },
    Expr { value: Box<Expr> },
    Pass,
    Break,
    Continue,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ExprKind {
    BoolOp { op: BoolOp, values: Vec<Expr> },
    NamedExpr { target: Box<Expr>, value: Box<Expr> },
    BinOp { left: Box<Expr>, op: Operator, right: Box<Expr> },
    UnaryOp { op: UnaryOp, operand: Box<Expr> },
    Lambda { args: Box<Arguments>, body: Box<Expr> },
    IfExp { test: Box<Expr>, body: Box<Expr>, orelse: Box<Expr> },
    Dict { keys: Vec<Option<Expr>>, values: Vec<Expr> },
    Set { elts: Vec<Expr> },
    ListComp { elt: Box<Expr>, generators: Vec<Comprehension> },
    SetComp { elt: Box<Expr>, generators: Vec<Comprehension> },
    DictComp { key: Box<Expr>, value: Box<Expr>, generators: Vec<Comprehension> },
    GeneratorExp { elt: Box<Expr>, generators: Vec<Comprehension> },
    Await { value: Box<Expr> },
    Yield { value: Option<Box<Expr>> },
    YieldFrom { value: Box<Expr> },
    Compare { left: Box<Expr>, ops: Vec<CmpOp>, comparators: Vec<Expr> },
    Call { func: Box<Expr>, args: Vec<Expr>, keywords: Vec<Keyword> },
    FormattedValue { value: Box<Expr>, conversion: i32, format_spec: Option<Box<Expr>> },
    Interpolation { value: Box<Expr>, str: String, conversion: i32, format_spec: Option<Box<Expr>> },
    JoinedStr { values: Vec<Expr> },
    TemplateStr { values: Vec<Expr> },
    Constant { value: Constant, kind: Option<String> },
    Attribute { value: Box<Expr>, attr: String, ctx: ExprContext },
    Subscript { value: Box<Expr>, slice: Box<Expr>, ctx: ExprContext },
    Starred { value: Box<Expr>, ctx: ExprContext },
    Name { id: String, ctx: ExprContext },
    List { elts: Vec<Expr>, ctx: ExprContext },
    Tuple { elts: Vec<Expr>, ctx: ExprContext },
    Slice { lower: Option<Box<Expr>>, upper: Option<Box<Expr>>, step: Option<Box<Expr>> },
}

/// Literal value of a `Constant` node.
#[derive(Debug, Clone, PartialEq)]
pub enum Constant {
    None,
    Bool(bool),
    /// Arbitrary precision integer as a decimal string with optional leading `-`.
    Int(String),
    Float(f64),
    /// Imaginary literal; the value is the imaginary part.
    Complex(f64),
    Str(String),
    Bytes(Vec<u8>),
    Ellipsis,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExprContext { Load, Store, Del }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BoolOp { And, Or }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Operator { Add, Sub, Mult, MatMult, Div, Mod, Pow, LShift, RShift, BitOr, BitXor, BitAnd, FloorDiv }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp { Invert, Not, UAdd, USub }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CmpOp { Eq, NotEq, Lt, LtE, Gt, GtE, Is, IsNot, In, NotIn }

#[derive(Debug, Clone, PartialEq)]
pub struct Comprehension {
    pub target: Expr,
    pub iter: Expr,
    pub ifs: Vec<Expr>,
    pub is_async: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ExceptHandler {
    pub type_: Option<Box<Expr>>,
    pub name: Option<String>,
    pub body: Vec<Stmt>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Arguments {
    pub posonlyargs: Vec<Arg>,
    pub args: Vec<Arg>,
    pub vararg: Option<Box<Arg>>,
    pub kwonlyargs: Vec<Arg>,
    pub kw_defaults: Vec<Option<Expr>>,
    pub kwarg: Option<Box<Arg>>,
    pub defaults: Vec<Expr>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Arg {
    pub arg: String,
    pub annotation: Option<Box<Expr>>,
    pub type_comment: Option<String>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Keyword {
    pub arg: Option<String>,
    pub value: Expr,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Alias {
    pub name: String,
    pub asname: Option<String>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub struct WithItem {
    pub context_expr: Expr,
    pub optional_vars: Option<Expr>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct MatchCase {
    pub pattern: Pattern,
    pub guard: Option<Box<Expr>>,
    pub body: Vec<Stmt>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Pattern {
    pub kind: PatternKind,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub enum PatternKind {
    MatchValue { value: Box<Expr> },
    MatchSingleton { value: Constant },
    MatchSequence { patterns: Vec<Pattern> },
    MatchMapping { keys: Vec<Expr>, patterns: Vec<Pattern>, rest: Option<String> },
    MatchClass { cls: Box<Expr>, patterns: Vec<Pattern>, kwd_attrs: Vec<String>, kwd_patterns: Vec<Pattern> },
    MatchStar { name: Option<String> },
    MatchAs { pattern: Option<Box<Pattern>>, name: Option<String> },
    MatchOr { patterns: Vec<Pattern> },
}

#[derive(Debug, Clone, PartialEq)]
pub struct TypeParam {
    pub kind: TypeParamKind,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub enum TypeParamKind {
    TypeVar { name: String, bound: Option<Box<Expr>>, default_value: Option<Box<Expr>> },
    ParamSpec { name: String, default_value: Option<Box<Expr>> },
    TypeVarTuple { name: String, default_value: Option<Box<Expr>> },
}
