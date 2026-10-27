//! Lexical analyzer supporting ranges (`..`, `..=`), duration literals (`1s`, `500ms`), and string interpolation.
use rhea_span::Span;

#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    Int(i64),
    Float(f64),
    DurationMs(i64),
    Str(String),
    Ident(String),
    DotDot,
    DotDotEq,
    PipeGreater,
    Arrow,
    FatArrow,
    Eof,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub span: Span,
}
