"""Inference wrapper for trained Rhea AI models."""
from __future__ import annotations

from .models import MLP2Layer
from .tensors import Tensor

class Predictor:
    def __init__(self, model: MLP2Layer) -> None:
        self.model = model

    def predict(self, x: Tensor) -> list[list[float]]:
        out = self.model.forward(x)
        return out.to_list()
