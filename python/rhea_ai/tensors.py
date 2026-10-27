"""High-level Tensor wrapper backed by Rhea C++20 native kernels."""
from __future__ import annotations

import ctypes
from typing import Any, Sequence
from .bridge import _CTensorF64, NativeTensor, check_status, get_lib

def _flatten_and_infer_shape(data: Any) -> tuple[list[float], tuple[int, ...]]:
    if hasattr(data, "tolist") and hasattr(data, "shape"):
        shape = tuple(int(d) for d in data.shape)
        flat = [float(x) for x in data.reshape(-1).tolist()]
        return flat, shape
    if isinstance(data, (int, float)):
        return [float(data)], (1,)
    if isinstance(data, (list, tuple)):
        if len(data) == 0:
            raise ValueError("Cannot create Tensor from empty list")
        if isinstance(data[0], (list, tuple)):
            rows = len(data)
            cols = len(data[0])
            flat: list[float] = []
            for r in data:
                if len(r) != cols:
                    raise ValueError("Ragged nested lists are not supported")
                flat.extend(float(v) for v in r)
            return flat, (rows, cols)
        return [float(v) for v in data], (len(data),)
    raise TypeError(f"Unsupported tensor input type: {type(data)}")

class Tensor:
    """High-level tensor supporting arithmetic, matrix multiplication, activations, and reductions."""
    def __init__(self, data: Any, shape: Sequence[int] | None = None) -> None:
        if isinstance(data, NativeTensor):
            self._native = data
            return
        flat, inferred_shape = _flatten_and_infer_shape(data)
        actual_shape = tuple(int(d) for d in shape) if shape is not None else inferred_shape
        self._native = NativeTensor.from_flat(flat, actual_shape)

    @property
    def shape(self) -> tuple[int, ...]:
        return self._native.shape

    @property
    def numel(self) -> int:
        return self._native.numel

    def to_list(self) -> list[Any]:
        flat = self._native.to_flat_list()
        if len(self.shape) == 1:
            return flat
        if len(self.shape) == 2:
            rows, cols = self.shape
            return [flat[r * cols : (r + 1) * cols] for r in range(rows)]
        return flat

    def to_flat(self) -> list[float]:
        return self._native.to_flat_list()

    def _binary_op(self, other: "Tensor", fn_name: str) -> "Tensor":
        lib = get_lib()
        raw = _CTensorF64()
        fn = getattr(lib, fn_name)
        check_status(fn(ctypes.byref(self._native._c), ctypes.byref(other._native._c), ctypes.byref(raw)))
        return Tensor(NativeTensor(raw))

    def _unary_op(self, fn_name: str) -> "Tensor":
        lib = get_lib()
        raw = _CTensorF64()
        fn = getattr(lib, fn_name)
        check_status(fn(ctypes.byref(self._native._c), ctypes.byref(raw)))
        return Tensor(NativeTensor(raw))

    def __add__(self, other: "Tensor") -> "Tensor":
        return self._binary_op(other, "rhea_tensor_add")

    def __sub__(self, other: "Tensor") -> "Tensor":
        return self._binary_op(other, "rhea_tensor_sub")

    def __mul__(self, other: "Tensor") -> "Tensor":
        return self._binary_op(other, "rhea_tensor_mul")

    def matmul(self, other: "Tensor") -> "Tensor":
        return self._binary_op(other, "rhea_tensor_matmul")

    def __matmul__(self, other: "Tensor") -> "Tensor":
        return self.matmul(other)

    def transpose(self) -> "Tensor":
        return self._unary_op("rhea_tensor_transpose_2d")

    def relu(self) -> "Tensor":
        return self._unary_op("rhea_tensor_relu")

    def sigmoid(self) -> "Tensor":
        return self._unary_op("rhea_tensor_sigmoid")

    def tanh(self) -> "Tensor":
        return self._unary_op("rhea_tensor_tanh")

    def softmax(self) -> "Tensor":
        return self._unary_op("rhea_tensor_softmax_last_dim")

    def sum(self) -> float:
        lib = get_lib()
        out = ctypes.c_double(0.0)
        check_status(lib.rhea_tensor_sum(ctypes.byref(self._native._c), ctypes.byref(out)))
        return float(out.value)

    def mean(self) -> float:
        lib = get_lib()
        out = ctypes.c_double(0.0)
        check_status(lib.rhea_tensor_mean(ctypes.byref(self._native._c), ctypes.byref(out)))
        return float(out.value)

    def l2_norm(self) -> float:
        lib = get_lib()
        out = ctypes.c_double(0.0)
        check_status(lib.rhea_tensor_l2_norm(ctypes.byref(self._native._c), ctypes.byref(out)))
        return float(out.value)
