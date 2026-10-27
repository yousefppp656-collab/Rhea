"""Rhea AI Python Integration Layer (v0.12.0).
Provides seamless interoperability between Rhea's C++20 native numerical/autodiff kernels
and Python (works with NumPy when installed, or pure-Python ctypes buffers out of the box).
"""
from __future__ import annotations

from .bridge import abi_version, NativeTensor, status_message
from .tensors import Tensor
from .datasets import make_xor_dataset, make_spiral_dataset, make_linear_regression_dataset
from .models import MLP2Layer
from .training import Trainer, TrainingMetrics
from .inference import Predictor

__version__ = "0.12.0"
__all__ = [
    "abi_version",
    "NativeTensor",
    "status_message",
    "Tensor",
    "make_xor_dataset",
    "make_spiral_dataset",
    "make_linear_regression_dataset",
    "MLP2Layer",
    "Trainer",
    "TrainingMetrics",
    "Predictor",
]
