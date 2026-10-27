//! Beginner-friendly compiler and runtime diagnostics for Rhea v0.9.3.
use rhea_span::Span;

#[derive(Debug, Clone)]
pub struct Diagnostic {
    pub code: &'static str,
    pub message: String,
    pub span: Span,
    pub why: String,
    pub help: String,
    pub example: String,
}

impl Diagnostic {
    pub fn render(&self, file: &str, source: &str) -> String {
        let line_str = source.lines().nth(self.span.line.saturating_sub(1)).unwrap_or("");
        format!(
            "error[{}]: {}\n  --> {}:{}:{}\n   |\n{:>3} | {}\n   | {}^\n   = why: {}\n   = help: {}\n   = example: {}\n",
            self.code, self.message, file, self.span.line, self.span.col,
            self.span.line, line_str, " ".repeat(self.span.col.saturating_sub(1)),
            self.why, self.help, self.example
        )
    }
}
