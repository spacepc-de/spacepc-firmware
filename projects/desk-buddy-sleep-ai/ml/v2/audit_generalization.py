#!/usr/bin/env python3
"""Audit a binary model by unseen person/night group and recording source."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import torch
from sklearn.metrics import average_precision_score
from torch.utils.data import DataLoader

from train_binary import BinaryDataset, BinaryStudent, BinaryStudentMedium, predict


def classification(target: np.ndarray, probability: np.ndarray, threshold: float) -> dict[str, float | int | None]:
    target = target.astype(bool)
    selected = probability >= threshold
    tp = int((target & selected).sum())
    fp = int((~target & selected).sum())
    fn = int((target & ~selected).sum())
    tn = int((~target & ~selected).sum())
    precision = tp / max(tp + fp, 1)
    recall = tp / max(tp + fn, 1)
    return {
        "windows": int(len(target)), "positives": int(target.sum()),
        "threshold": threshold, "precision": precision, "recall": recall,
        "specificity": tn / max(tn + fp, 1),
        "f1": 2 * precision * recall / max(precision + recall, 1e-9),
        "average_precision": float(average_precision_score(target, probability))
        if len(np.unique(target)) == 2 else None,
        "mean_probability": float(probability.mean()),
        "median_probability": float(np.median(probability)),
        "tp": tp, "fp": fp, "fn": fn, "tn": tn,
    }


@torch.no_grad()
def score_field(model: torch.nn.Module, device: torch.device, path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    archive = np.load(path)
    feature = np.asarray(archive["features"], np.float32).transpose(0, 3, 1, 2)
    result = []
    for start in range(0, len(feature), 128):
        value = torch.from_numpy(feature[start:start + 128]).to(device)
        result.append(model(value).sigmoid().cpu().numpy())
    return np.concatenate(result), archive["labels"].astype(np.int8), archive["groups"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--features", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--field", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    metadata = json.loads(args.metadata.read_text())
    threshold = float(metadata["threshold"])
    snore_id = metadata["labels"].index("snore")
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    model_type = BinaryStudentMedium if metadata.get("architecture") == "medium" else BinaryStudent
    model = model_type().to(device)
    model.load_state_dict(torch.load(args.model, map_location=device, weights_only=True))
    model.eval()

    splits = np.load(args.features / "splits.npy")
    groups = np.load(args.features / "groups.npy")
    sources = np.load(args.features / "sources.npy")
    labels = np.load(args.features / "labels.npy", mmap_mode="r")
    group_splits: dict[str, set[str]] = {}
    for group, split in zip(groups, splits):
        group_splits.setdefault(str(group), set()).add(str(split))
    leaking = {group: sorted(value) for group, value in group_splits.items() if len(value) > 1}

    report: dict[str, object] = {
        "device": str(device), "threshold": threshold,
        "group_leakage": leaking, "splits": {},
    }
    high = int(np.searchsorted(metadata["mel_frequencies"], 2000, side="right"))
    for split in ("validation", "test"):
        indices = np.flatnonzero(splits == split)
        dataset = BinaryDataset(args.features, indices, high, False, snore_id=snore_id)
        probability, target = predict(model, DataLoader(dataset, batch_size=128), device)
        source_report = {}
        for source in sorted(np.unique(sources[indices])):
            selected = sources[indices] == source
            source_report[str(source)] = {
                "groups": int(len(np.unique(groups[indices][selected]))),
                **classification(target[selected], probability[selected], threshold),
            }
        report["splits"][split] = {
            "groups": int(len(np.unique(groups[indices]))),
            "overall": classification(target, probability, threshold),
            "sources": source_report,
        }

    if args.field:
        probability, target, field_groups = score_field(model, device, args.field)
        recordings = {}
        for group in sorted(np.unique(field_groups)):
            selected = field_groups == group
            recordings[str(group)] = classification(target[selected], probability[selected], threshold)
        report["field"] = {
            "overall": classification(target, probability, threshold),
            "recordings": recordings,
            "warning": "Same-person field clips are a domain audit, not a global release test.",
        }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
