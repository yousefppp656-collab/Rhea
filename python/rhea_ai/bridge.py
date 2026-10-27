"""Low-level ctypes bridge to librhea_native.so (ABI v2)."""
from __future__ import annotations

import ctypes
from ctypes import c_char_p, c_double, c_int, c_size_t, c_uint32, c_uint64, POINTER, Structure
from pathlib import Path
from typing import Sequence

class RheaNativeError(RuntimeError):
    """Raised when a native C++20 kernel returns a non-zero rhea_Status."""

class _CTensorF64(Structure):
    _fields_ = [
        ("data", POINTER(c_double)),
        ("shape", POINTER(c_size_t)),
        ("ndim", c_size_t),
        ("numel", c_size_t),
    ]

class _CMLP2Layer(Structure):
    _fields_ = [
        ("in_dim", c_size_t),
        ("hidden_dim", c_size_t),
        ("out_dim", c_size_t),
        ("w1", _CTensorF64),
        ("b1", _CTensorF64),
        ("w2", _CTensorF64),
        ("b2", _CTensorF64),
        ("m_w1", _CTensorF64),
        ("v_w1", _CTensorF64),
        ("m_b1", _CTensorF64),
        ("v_b1", _CTensorF64),
        ("m_w2", _CTensorF64),
        ("v_w2", _CTensorF64),
        ("m_b2", _CTensorF64),
        ("v_b2", _CTensorF64),
        ("step", c_uint64),
    ]

def _load_library() -> ctypes.CDLL:
    root = Path(__file__).resolve().parents[2]
    candidates = [
        root / "lib" / "librhea_native.so",
        root / "lib" / "librhea_native.dylib",
    ]
    for path in candidates:
        if path.exists():
            lib = ctypes.CDLL(str(path))
            _configure_signatures(lib)
            return lib
    raise FileNotFoundError(f"Could not find librhea_native.so in {root / 'lib'}")

def _configure_signatures(lib: ctypes.CDLL) -> None:
    lib.rhea_abi_version.argtypes = []
    lib.rhea_abi_version.restype = c_uint32

    lib.rhea_status_message.argtypes = [c_int]
    lib.rhea_status_message.restype = c_char_p

    lib.rhea_tensor_zeros.argtypes = [POINTER(c_size_t), c_size_t, POINTER(_CTensorF64)]
    lib.rhea_tensor_zeros.restype = c_int

    lib.rhea_tensor_from_buffer.argtypes = [
        POINTER(c_double), c_size_t, POINTER(c_size_t), c_size_t, POINTER(_CTensorF64)
    ]
    lib.rhea_tensor_from_buffer.restype = c_int

    lib.rhea_tensor_free.argtypes = [POINTER(_CTensorF64)]
    lib.rhea_tensor_free.restype = None

    for fn_name in ("rhea_tensor_add", "rhea_tensor_sub", "rhea_tensor_mul", "rhea_tensor_matmul"):
        fn = getattr(lib, fn_name)
        fn.argtypes = [POINTER(_CTensorF64), POINTER(_CTensorF64), POINTER(_CTensorF64)]
        fn.restype = c_int

    for fn_name in (
        "rhea_tensor_transpose_2d",
        "rhea_tensor_relu",
        "rhea_tensor_sigmoid",
        "rhea_tensor_tanh",
        "rhea_tensor_softmax_last_dim",
    ):
        fn = getattr(lib, fn_name)
        fn.argtypes = [POINTER(_CTensorF64), POINTER(_CTensorF64)]
        fn.restype = c_int

    lib.rhea_tensor_scale.argtypes = [POINTER(_CTensorF64), c_double, POINTER(_CTensorF64)]
    lib.rhea_tensor_scale.restype = c_int

    for fn_name in ("rhea_tensor_sum", "rhea_tensor_mean", "rhea_tensor_l2_norm"):
        fn = getattr(lib, fn_name)
        fn.argtypes = [POINTER(_CTensorF64), POINTER(c_double)]
        fn.restype = c_int

    lib.rhea_mse_loss_and_grad.argtypes = [
        POINTER(_CTensorF64), POINTER(_CTensorF64), POINTER(c_double), POINTER(_CTensorF64)
    ]
    lib.rhea_mse_loss_and_grad.restype = c_int

    lib.rhea_mlp_create.argtypes = [c_size_t, c_size_t, c_size_t, c_uint64, POINTER(_CMLP2Layer)]
    lib.rhea_mlp_create.restype = c_int

    lib.rhea_mlp_free.argtypes = [POINTER(_CMLP2Layer)]
    lib.rhea_mlp_free.restype = None

    lib.rhea_mlp_forward.argtypes = [POINTER(_CMLP2Layer), POINTER(_CTensorF64), POINTER(_CTensorF64)]
    lib.rhea_mlp_forward.restype = c_int

    lib.rhea_mlp_train_step.argtypes = [
        POINTER(_CMLP2Layer), POINTER(_CTensorF64), POINTER(_CTensorF64), c_double, POINTER(c_double)
    ]
    lib.rhea_mlp_train_step.restype = c_int

    lib.rhea_mlp_train_epochs.argtypes = [
        POINTER(_CMLP2Layer), POINTER(_CTensorF64), POINTER(_CTensorF64),
        c_double, c_size_t, POINTER(c_double), POINTER(c_double)
    ]
    lib.rhea_mlp_train_epochs.restype = c_int

    lib.rhea_mlp_save_checkpoint.argtypes = [POINTER(_CMLP2Layer), c_char_p]
    lib.rhea_mlp_save_checkpoint.restype = c_int

    lib.rhea_mlp_load_checkpoint.argtypes = [POINTER(_CMLP2Layer), c_char_p]
    lib.rhea_mlp_load_checkpoint.restype = c_int

_LIB = None

def get_lib() -> ctypes.CDLL:
    global _LIB
    if _LIB is None:
        _LIB = _load_library()
    return _LIB

def status_message(code: int) -> str:
    msg = get_lib().rhea_status_message(code)
    return msg.decode("utf-8") if msg else f"Status({code})"

def check_status(code: int) -> None:
    if code != 0:
        raise RheaNativeError(f"Rhea native error [{code}]: {status_message(code)}")

def abi_version() -> int:
    return int(get_lib().rhea_abi_version())

class NativeTensor:
    """RAII wrapper around rhea_TensorF64."""
    def __init__(self, c_tensor: _CTensorF64) -> None:
        self._c = c_tensor
        self._freed = False

    @classmethod
    def from_flat(cls, flat_data: Sequence[float], shape: Sequence[int]) -> "NativeTensor":
        lib = get_lib()
        ndim = len(shape)
        numel = len(flat_data)
        c_shape = (c_size_t * ndim)(*shape)
        c_buf = (c_double * numel)(*[float(v) for v in flat_data])
        raw = _CTensorF64()
        check_status(lib.rhea_tensor_from_buffer(c_buf, numel, c_shape, ndim, ctypes.byref(raw)))
        return cls(raw)

    @property
    def shape(self) -> tuple[int, ...]:
        return tuple(int(self._c.shape[i]) for i in range(self._c.ndim))

    @property
    def numel(self) -> int:
        return int(self._c.numel)

    def to_flat_list(self) -> list[float]:
        return [float(self._c.data[i]) for i in range(self._c.numel)]

    def close(self) -> None:
        if not self._freed and _LIB is not None:
            _LIB.rhea_tensor_free(ctypes.byref(self._c))
            self._freed = True

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
