"""Neural network models backed by Rhea C++20 native kernels."""
from __future__ import annotations

import ctypes
from pathlib import Path
from .bridge import _CMLP2Layer, _CTensorF64, NativeTensor, check_status, get_lib
from .tensors import Tensor

class MLP2Layer:
    """Two-layer Multi-Layer Perceptron with Adam optimizer and checksummed binary persistence."""
    def __init__(self, in_dim: int, hidden_dim: int, out_dim: int, seed: int = 42, _raw: _CMLP2Layer | None = None) -> None:
        self._freed = False
        if _raw is not None:
            self._c = _raw
        else:
            self._c = _CMLP2Layer()
            check_status(get_lib().rhea_mlp_create(in_dim, hidden_dim, out_dim, seed, ctypes.byref(self._c)))

    @property
    def in_dim(self) -> int:
        return int(self._c.in_dim)

    @property
    def hidden_dim(self) -> int:
        return int(self._c.hidden_dim)

    @property
    def out_dim(self) -> int:
        return int(self._c.out_dim)

    def forward(self, x: Tensor) -> Tensor:
        raw_out = _CTensorF64()
        check_status(get_lib().rhea_mlp_forward(ctypes.byref(self._c), ctypes.byref(x._native._c), ctypes.byref(raw_out)))
        return Tensor(NativeTensor(raw_out))

    def train_step(self, x: Tensor, targets: Tensor, lr: float = 0.02) -> float:
        loss = ctypes.c_double(0.0)
        check_status(
            get_lib().rhea_mlp_train_step(
                ctypes.byref(self._c),
                ctypes.byref(x._native._c),
                ctypes.byref(targets._native._c),
                lr,
                ctypes.byref(loss),
            )
        )
        return float(loss.value)

    def train_epochs(self, x: Tensor, targets: Tensor, lr: float = 0.02, epochs: int = 200) -> tuple[float, float]:
        init_loss = ctypes.c_double(0.0)
        final_loss = ctypes.c_double(0.0)
        check_status(
            get_lib().rhea_mlp_train_epochs(
                ctypes.byref(self._c),
                ctypes.byref(x._native._c),
                ctypes.byref(targets._native._c),
                lr,
                epochs,
                ctypes.byref(init_loss),
                ctypes.byref(final_loss),
            )
        )
        return float(init_loss.value), float(final_loss.value)

    def save(self, path: str | Path) -> None:
        b_path = str(path).encode("utf-8")
        check_status(get_lib().rhea_mlp_save_checkpoint(ctypes.byref(self._c), b_path))

    @classmethod
    def load(cls, path: str | Path) -> "MLP2Layer":
        b_path = str(path).encode("utf-8")
        raw = _CMLP2Layer()
        check_status(get_lib().rhea_mlp_load_checkpoint(ctypes.byref(raw), b_path))
        return cls(int(raw.in_dim), int(raw.hidden_dim), int(raw.out_dim), _raw=raw)

    def close(self) -> None:
        if not self._freed:
            get_lib().rhea_mlp_free(ctypes.byref(self._c))
            self._freed = True

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
