#!/usr/bin/env python3
"""Evaluate every overlapping window of labeled Waveshare recordings."""
from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np
import torch

from prepare_features import harmonic_representation, load_window, representation
from train_binary import BinaryStudent, BinaryStudentMedium


HOP = 0.5


def duration(path: Path) -> float:
    with wave.open(str(path), "rb") as stream:
        return stream.getnframes() / stream.getframerate()


@torch.no_grad()
def score(model, path: Path, device: torch.device, channel: str = "2", representation_name: str = "masked") -> np.ndarray:
    starts = np.arange(0, max(0.001, duration(path) - 2.0 + 0.001), HOP)
    result = []
    for first in range(0, len(starts), 64):
        batch = []
        for start in starts[first:first + 64]:
            extractor = harmonic_representation if representation_name == "harmonic" else representation
            feature, mask = extractor(load_window(path, float(start), channel), 24000)
            batch.append(np.stack((feature, mask), axis=0))
        result.extend(model(torch.from_numpy(np.asarray(batch, np.float32)).to(device)).sigmoid().cpu().tolist())
    return np.asarray(result)


def event_count(probability: np.ndarray, threshold: float) -> int:
    active = False; above = below = events = 0
    for value in probability:
        if not active:
            above = above + 1 if value >= threshold else 0
            if above >= 2: active = True; below = 0
        else:
            below = below + 1 if value < threshold * 0.65 else 0
            if below >= 3: events += 1; active = False; above = below = 0
    return events + int(active)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recordings", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text()); threshold = float(metadata["threshold"])
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    model_class = BinaryStudentMedium if metadata.get("architecture") == "medium" else BinaryStudent
    model = model_class().to(device)
    model.load_state_dict(torch.load(args.model, map_location=device, weights_only=True)); model.eval()
    files = [(args.recordings / "snore-board-001.wav", 1),
             (args.recordings / "breathing-board-001.wav", 0)]
    totals = dict(tp=0, fp=0, fn=0, tn=0); recordings = []
    for path, target in files:
        probability = score(model, path, device, representation_name=metadata.get("representation", "masked")); selected = probability >= threshold
        if target:
            totals["tp"] += int(selected.sum()); totals["fn"] += int((~selected).sum())
        else:
            totals["fp"] += int(selected.sum()); totals["tn"] += int((~selected).sum())
        recordings.append({"path": str(path), "target": "snore" if target else "not_snore",
                           "windows": len(probability), "mean_probability": float(probability.mean()),
                           "maximum_probability": float(probability.max()),
                           "positive_windows": int(selected.sum()), "events": event_count(probability, threshold)})
    precision = totals["tp"] / max(totals["tp"] + totals["fp"], 1)
    recall = totals["tp"] / max(totals["tp"] + totals["fn"], 1)
    result = {"threshold": threshold, "precision": precision, "recall": recall,
              "f1": 2 * precision * recall / max(precision + recall, 1e-9),
              "totals": totals, "recordings": recordings,
              "independent_night_required": True}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__": main()
