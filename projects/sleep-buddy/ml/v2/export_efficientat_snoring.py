#!/usr/bin/env python3
"""Reduce EfficientAT MN04 to the original AudioSet Snoring logit and export ONNX."""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
from torch import nn


class SnoringOnly(nn.Module):
    def __init__(self, features: nn.Module, classifier: nn.Module):
        super().__init__()
        self.features = features
        self.classifier = classifier

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.classifier(self.features(value))


def export_safe_se_scale(self: nn.Module, value: torch.Tensor) -> torch.Tensor:
    """Equivalent EfficientAT SE math without ONNX's ambiguous chained Squeeze."""
    scale = torch.mean(value, self.se_dim, keepdim=True)
    shape = scale.shape
    scale = torch.flatten(scale, start_dim=1)
    scale = self.activation(self.fc1(scale))
    scale = self.scale_activation(self.fc2(scale))
    return torch.reshape(scale, shape)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--efficientat-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", default="mn04_as")
    parser.add_argument("--frames", type=int, default=200)
    args = parser.parse_args()

    sys.path.insert(0, str(args.efficientat_root))
    os.chdir(args.efficientat_root)
    from helpers.utils import NAME_TO_WIDTH, labels  # noqa: PLC0415
    from models.mn.block_types import SqueezeExcitation  # noqa: PLC0415
    from models.mn.model import get_model  # noqa: PLC0415

    # Keep the model numerically identical while emitting a converter-friendly
    # Flatten/Reshape pair instead of two axis-sensitive Squeeze operations.
    SqueezeExcitation._scale = export_safe_se_scale

    torch.manual_seed(260810)
    base = get_model(width_mult=NAME_TO_WIDTH(args.model), pretrained_name=args.model,
                     strides=[2, 2, 2, 2], input_dim_t=args.frames).eval()
    snoring_index = labels.index("Snoring")
    original = base.classifier[5]
    reduced = nn.Linear(original.in_features, 1)
    with torch.no_grad():
        reduced.weight.copy_(original.weight[snoring_index:snoring_index + 1])
        reduced.bias.copy_(original.bias[snoring_index:snoring_index + 1])
    base.classifier[5] = reduced
    model = SnoringOnly(base.features, base.classifier).eval()

    args.output.mkdir(parents=True, exist_ok=True)
    checkpoint = args.output / "efficientat_mn04_snoring.pt"
    onnx_path = args.output / "efficientat_mn04_snoring.onnx"
    torch.save(model.state_dict(), checkpoint)

    example = torch.rand(1, 1, 128, args.frames)
    with torch.no_grad():
        expected = model(example).numpy()
    torch.onnx.export(
        model, example, onnx_path, input_names=["logmel"], output_names=["snoring_logit"],
        opset_version=18, dynamo=False,
    )
    graph = onnx.load(onnx_path)
    onnx.checker.check_model(graph)
    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    actual = session.run(None, {"logmel": example.numpy()})[0]
    max_error = float(np.max(np.abs(expected - actual)))
    if max_error > 1e-4:
        raise RuntimeError(f"ONNX parity failed: max error {max_error}")

    report = {
        "source_model": args.model,
        "source_class": "Snoring",
        "source_class_index": snoring_index,
        "input": [1, 1, 128, args.frames],
        "output": [1, 1],
        "parameters": sum(parameter.numel() for parameter in model.parameters()),
        "checkpoint_bytes": checkpoint.stat().st_size,
        "onnx_bytes": onnx_path.stat().st_size,
        "onnx_max_abs_error": max_error,
    }
    (args.output / "export_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
