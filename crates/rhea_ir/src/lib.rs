//! Stack-based bytecode instruction set for the Rhea Virtual Machine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpCode {
    LoadConst, LoadLocal, StoreLocal,
    Add, Sub, Mul, Div, Mod,
    RangeExcl, RangeIncl,
    SleepMs, InputLine,
    Jump, JumpIfFalse, Call, Return, Halt,
}
