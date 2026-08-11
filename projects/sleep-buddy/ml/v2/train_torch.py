#!/usr/bin/env python3
"""Train the Sleep AI teacher/student pair natively with PyTorch and Apple Metal."""
from __future__ import annotations

import argparse
import json
import os
import random
from pathlib import Path

import numpy as np
import torch
from sklearn.metrics import classification_report, confusion_matrix, precision_recall_curve
from torch import nn
from torch.utils.data import DataLoader, Dataset


SEED = 260810


class FeatureDataset(Dataset):
    def __init__(self, root: Path, indices: np.ndarray, high_start: int, augment: bool = False,
                 soft_targets: np.ndarray | None = None):
        self.features = np.load(root / "features.npy", mmap_mode="r")
        self.masks = np.load(root / "masks.npy", mmap_mode="r")
        self.labels = np.load(root / "labels.npy", mmap_mode="r")
        self.weights = np.load(root / "weights.npy", mmap_mode="r")
        self.indices = np.asarray(indices)
        self.high_start = high_start
        self.augment = augment
        self.soft_targets = soft_targets

    def __len__(self) -> int:
        return len(self.indices)

    def __getitem__(self, item: int):
        index = self.indices[item]
        # CNN input is channels x time x mel. Copy keeps the memory-mapped source immutable.
        value = np.stack((self.features[index], self.masks[index]), axis=0).astype(np.float32)
        if self.augment and random.random() < 0.35:
            value[:, :, self.high_start:] = 0.0
        if self.augment and random.random() < 0.25:
            width = random.randint(3, 10)
            start = random.randint(0, value.shape[1] - width)
            value[0, start:start + width, :] = 0.0
        if self.augment and random.random() < 0.25:
            width = random.randint(2, 6)
            start = random.randint(0, value.shape[2] - width)
            value[0, :, start:start + width] = 0.0
        if self.augment and random.random() < 0.50:
            # Smooth random frequency response emulates unseen microphones and
            # enclosures. Apply the same physical response to both channels.
            anchors = np.random.uniform(-0.10, 0.10, 6).astype(np.float32)
            response = np.interp(np.arange(value.shape[2]),
                                 np.linspace(0, value.shape[2] - 1, len(anchors)), anchors).astype(np.float32)
            value = np.clip(value + response[None, None, :], 0.0, 1.0).astype(np.float32)
        target = int(self.labels[index]) if self.soft_targets is None else self.soft_targets[item]
        return torch.from_numpy(value), torch.as_tensor(target), torch.tensor(float(self.weights[index]))


class SeparableBlock(nn.Module):
    def __init__(self, source: int, target: int, stride: int = 1):
        super().__init__()
        self.block = nn.Sequential(
            nn.Conv2d(source, source, 3, stride=stride, padding=1, groups=source, bias=False),
            nn.BatchNorm2d(source), nn.SiLU(),
            nn.Conv2d(source, target, 1, bias=False), nn.BatchNorm2d(target), nn.SiLU())

    def forward(self, value):
        return self.block(value)


class Teacher(nn.Module):
    def __init__(self, classes: int):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(2, 32, 5, stride=2, padding=2, bias=False), nn.BatchNorm2d(32), nn.SiLU(),
            SeparableBlock(32, 48, 2), SeparableBlock(48, 64, 2), SeparableBlock(64, 96, 2))
        self.head = nn.Sequential(nn.Linear(192, 128), nn.SiLU(), nn.Dropout(0.30), nn.Linear(128, classes))

    def forward(self, value):
        value = self.features(value)
        pooled = torch.cat((value.mean((2, 3)), value.amax((2, 3))), dim=1)
        return self.head(pooled)


class Student(nn.Module):
    def __init__(self, classes: int):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(2, 12, (5, 3), stride=2, padding=(2, 1), bias=False), nn.BatchNorm2d(12), nn.ReLU(),
            SeparableBlock(12, 20, 2), SeparableBlock(20, 28, 2), SeparableBlock(28, 40, 1))
        self.head = nn.Sequential(nn.Linear(40, 32), nn.ReLU(), nn.Linear(32, classes))

    def forward(self, value):
        return self.head(self.features(value).mean((2, 3)))


def loader(dataset: Dataset, batch: int, shuffle: bool) -> DataLoader:
    # MPS shares system memory, so extra worker processes hurt an 8 GB machine.
    return DataLoader(dataset, batch_size=batch, shuffle=shuffle, num_workers=0, pin_memory=False)


def run_epoch(model, batches, device, optimizer=None, distill=False):
    training = optimizer is not None
    model.train(training)
    total_loss = total_weight = correct = 0.0
    for values, targets, weights in batches:
        values, targets, weights = values.to(device), targets.to(device), weights.to(device)
        with torch.set_grad_enabled(training):
            logits = model(values)
            if distill:
                losses = -(targets.to(torch.float32) * logits.log_softmax(1)).sum(1)
                hard = targets.argmax(1)
            else:
                losses = nn.functional.cross_entropy(logits, targets.long(), reduction="none")
                hard = targets.long()
            loss = (losses * weights).sum() / weights.sum().clamp_min(1e-6)
            if training:
                optimizer.zero_grad(set_to_none=True)
                loss.backward()
                optimizer.step()
        total_loss += float((losses * weights).sum().detach().cpu())
        total_weight += float(weights.sum().cpu())
        correct += float((logits.argmax(1) == hard).sum().cpu())
    return total_loss / max(total_weight, 1.0), correct / max(len(batches.dataset), 1)


def fit(model, train, validation, device, epochs: int, rate: float, patience: int, distill=False):
    optimizer = torch.optim.AdamW(model.parameters(), lr=rate, weight_decay=1e-4)
    best, stale, best_state = float("inf"), 0, None
    for epoch in range(1, epochs + 1):
        train_loss, train_accuracy = run_epoch(model, train, device, optimizer, distill)
        val_loss, val_accuracy = run_epoch(model, validation, device, distill=distill)
        print(f"epoch={epoch:03d} train_loss={train_loss:.4f} train_acc={train_accuracy:.4f} "
              f"val_loss={val_loss:.4f} val_acc={val_accuracy:.4f}", flush=True)
        if val_loss < best - 1e-4:
            best, stale = val_loss, 0
            best_state = {key: value.detach().cpu().clone() for key, value in model.state_dict().items()}
        else:
            stale += 1
            if stale >= patience:
                break
    model.load_state_dict(best_state)


@torch.no_grad()
def probabilities(model, batches, device) -> np.ndarray:
    model.eval()
    result = []
    for values, _, _ in batches:
        result.append(model(values.to(device)).softmax(1).cpu().numpy())
    return np.concatenate(result)


def threshold(y_true, probability, snore_id):
    precision, recall, thresholds = precision_recall_curve((y_true == snore_id).astype(np.int32), probability)
    f1 = 2 * precision * recall / np.maximum(precision + recall, 1e-9)
    index = int(np.nanargmax(f1[:-1])) if len(thresholds) else 0
    return {"threshold": float(thresholds[index]), "precision": float(precision[index]),
            "recall": float(recall[index]), "f1": float(f1[index])}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--features", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--teacher-epochs", type=int, default=60)
    parser.add_argument("--student-epochs", type=int, default=80)
    parser.add_argument("--batch", type=int, default=32)
    args = parser.parse_args()
    random.seed(SEED); np.random.seed(SEED); torch.manual_seed(SEED)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print(f"device={device} torch={torch.__version__} machine={os.uname().machine}")
    metadata = json.loads((args.features / "metadata.json").read_text())
    labels = metadata["labels"]
    y = np.load(args.features / "labels.npy")
    splits = np.load(args.features / "splits.npy")
    indices = {name: np.flatnonzero(splits == name) for name in ("train", "validation", "test")}
    if min(map(len, indices.values())) == 0:
        raise SystemExit(f"empty split: { {name: len(value) for name, value in indices.items()} }")
    high_start = int(np.searchsorted(metadata["mel_frequencies"], 2000.0, side="right"))
    base = {name: FeatureDataset(args.features, value, high_start, name == "train")
            for name, value in indices.items()}
    batches = {name: loader(value, args.batch, name == "train") for name, value in base.items()}

    teacher = Teacher(len(labels)).to(device)
    fit(teacher, batches["train"], batches["validation"], device, args.teacher_epochs, 7e-4, 10)
    teacher_soft = probabilities(teacher, loader(base["train"], args.batch, False), device)
    hard = np.eye(len(labels), dtype=np.float32)[y[indices["train"]]]
    targets = 0.65 * hard + 0.35 * teacher_soft
    student_train = FeatureDataset(args.features, indices["train"], high_start, True, targets)
    student_val_targets = np.eye(len(labels), dtype=np.float32)[y[indices["validation"]]]
    student_val = FeatureDataset(args.features, indices["validation"], high_start, False, student_val_targets)
    student = Student(len(labels)).to(device)
    fit(student, loader(student_train, args.batch, True), loader(student_val, args.batch, False), device,
        args.student_epochs, 8e-4, 12, True)

    args.output.mkdir(parents=True, exist_ok=True)
    torch.save(teacher.cpu().state_dict(), args.output / "teacher.pt")
    torch.save(student.cpu().state_dict(), args.output / "student.pt")
    test_probability = probabilities(student, batches["test"], torch.device("cpu"))
    validation_probability = probabilities(student, batches["validation"], torch.device("cpu"))
    predicted = test_probability.argmax(1)
    snore_id = labels.index("snore")
    selected = threshold(y[indices["validation"]], validation_probability[:, snore_id], snore_id)
    evaluation = {
        "device": str(device),
        "clip_report": classification_report(y[indices["test"]], predicted, labels=np.arange(len(labels)),
                                               target_names=labels, output_dict=True, zero_division=0),
        "confusion_matrix": confusion_matrix(y[indices["test"]], predicted,
                                               labels=np.arange(len(labels))).tolist(),
        "validation_snore_threshold": selected,
        "split_counts": {name: len(value) for name, value in indices.items()},
        "firmware_eligible": False,
        "reason": "event-level release gates not evaluated"
    }
    (args.output / "evaluation.json").write_text(json.dumps(evaluation, indent=2) + "\n")
    (args.output / "model_metadata.json").write_text(json.dumps({**metadata, **selected}, indent=2) + "\n")
    dummy = torch.zeros(1, 2, np.load(args.features / "features.npy", mmap_mode="r").shape[1],
                        metadata["mel_bands"])
    torch.onnx.export(student, dummy, args.output / "sleep_ai_student.onnx", input_names=["logmel_and_mask"],
                      output_names=["logits"], opset_version=18, dynamo=False,
                      dynamic_axes={"logmel_and_mask": {0: "batch"}, "logits": {0: "batch"}})
    print(json.dumps(evaluation, indent=2))


if __name__ == "__main__":
    main()
