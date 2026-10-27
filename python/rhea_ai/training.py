"""High-level training loop and convergence metrics for Rhea AI models."""
from __future__ import annotations

from dataclasses import dataclass
from .models import MLP2Layer
from .tensors import Tensor

@dataclass
class TrainingMetrics:
    initial_loss: float
    final_loss: float
    epochs: int
    converged: bool

class Trainer:
    def __init__(self, model: MLP2Layer, lr: float = 0.03) -> None:
        self.model = model
        self.lr = lr

    def fit(self, x: Tensor, y: Tensor, epochs: int = 400, target_loss: float = 0.01) -> TrainingMetrics:
        init_loss, final_loss = self.model.train_epochs(x, y, lr=self.lr, epochs=epochs)
        return TrainingMetrics(
            initial_loss=init_loss,
            final_loss=final_loss,
            epochs=epochs,
            converged=(final_loss <= target_loss),
        )
