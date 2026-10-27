"""Deterministic synthetic datasets for testing and benchmarking Rhea AI."""
from __future__ import annotations

import math
from .tensors import Tensor

def make_xor_dataset() -> tuple[Tensor, Tensor]:
    x = Tensor([[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]])
    y = Tensor([[0.0], [1.0], [1.0], [0.0]])
    return x, y

def make_linear_regression_dataset(n_samples: int = 32) -> tuple[Tensor, Tensor]:
    xs = []
    ys = []
    for i in range(n_samples):
        x1 = (i / max(1, n_samples - 1)) * 2.0 - 1.0
        x2 = math.sin(i * 0.5)
        y = 2.5 * x1 - 1.2 * x2 + 0.4
        xs.append([x1, x2])
        ys.append([y])
    return Tensor(xs), Tensor(ys)

def make_spiral_dataset(points_per_class: int = 20) -> tuple[Tensor, Tensor]:
    xs = []
    ys = []
    for cls in (0, 1):
        for i in range(points_per_class):
            r = i / max(1, points_per_class)
            theta = i * 0.3 + cls * math.pi
            xs.append([r * math.cos(theta), r * math.sin(theta)])
            ys.append([float(cls)])
    return Tensor(xs), Tensor(ys)
