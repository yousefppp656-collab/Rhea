//! Static type checker, bidirectional type inference, and implicit-immutable binding resolution.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Type {
    Int, Float, Bool, Str, Unit, Tensor, Any,
    List(Box<Type>),
    Map(Box<Type>, Box<Type>),
}
