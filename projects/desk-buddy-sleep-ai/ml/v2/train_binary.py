#!/usr/bin/env python3
"""Train a compact, source-balanced snore/not-snore detector."""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

import numpy as np
import torch
from sklearn.metrics import average_precision_score, precision_recall_curve
from torch import nn
from torch.utils.data import DataLoader, WeightedRandomSampler

from train_torch import FeatureDataset, SEED, SeparableBlock


class BinaryDataset(FeatureDataset):
    def __init__(self, *args, snore_id: int, soft_targets=None, **kwargs):
        super().__init__(*args, soft_targets=None, **kwargs)
        self.snore_id = snore_id
        self.binary_soft_targets = soft_targets
        self.sources = np.load(Path(self.features.filename).parent / "sources.npy", mmap_mode="r")

    def __getitem__(self, item):
        value, target, weight = super().__getitem__(item)
        binary = torch.tensor(float(target.item() == self.snore_id))
        if self.binary_soft_targets is not None:
            binary = torch.tensor(float(self.binary_soft_targets[item]))
        return value, binary, weight


class BinaryTeacher(nn.Module):
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(2, 32, 5, stride=2, padding=2, bias=False), nn.BatchNorm2d(32), nn.SiLU(),
            SeparableBlock(32, 48, 2), SeparableBlock(48, 64, 2), SeparableBlock(64, 96, 2))
        self.head = nn.Sequential(nn.Linear(192, 128), nn.SiLU(), nn.Dropout(0.3), nn.Linear(128, 1))

    def forward(self, value):
        value = self.features(value)
        return self.head(torch.cat((value.mean((2, 3)), value.amax((2, 3))), 1)).squeeze(1)


class BinaryStudent(nn.Module):
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(2, 12, (5, 3), stride=2, padding=(2, 1), bias=False), nn.BatchNorm2d(12), nn.ReLU(),
            SeparableBlock(12, 20, 2), SeparableBlock(20, 28, 2), SeparableBlock(28, 40, 1))
        self.head = nn.Sequential(nn.Linear(40, 32), nn.ReLU(), nn.Linear(32, 1))

    def forward(self, value):
        return self.head(self.features(value).mean((2, 3))).squeeze(1)


class BinaryStudentMedium(nn.Module):
    """Still tiny for ESP32-P4, but less bottlenecked than the 4.5k model."""
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(2, 16, (5, 3), stride=2, padding=(2, 1), bias=False), nn.BatchNorm2d(16), nn.ReLU(),
            SeparableBlock(16, 28, 2), SeparableBlock(28, 40, 2), SeparableBlock(40, 56, 1))
        self.head = nn.Sequential(nn.Linear(56, 48), nn.ReLU(), nn.Linear(48, 1))

    def forward(self, value):
        return self.head(self.features(value).mean((2, 3))).squeeze(1)


def make_loader(dataset, batch, shuffle=False, balanced=False):
    sampler = None
    if balanced:
        labels = (dataset.labels[dataset.indices] == dataset.snore_id).astype(np.int64)
        sources = dataset.sources[dataset.indices]
        sample_weights = np.zeros(len(labels), np.float64)
        for label in (0, 1):
            label_mask = labels == label
            domains = np.unique(sources[label_mask])
            for source in domains:
                cell = label_mask & (sources == source)
                sample_weights[cell] = 0.5 / (len(domains) * max(int(cell.sum()), 1))
        sampler = WeightedRandomSampler(torch.as_tensor(sample_weights, dtype=torch.double), len(labels), replacement=True)
        shuffle = False
    return DataLoader(dataset, batch_size=batch, shuffle=shuffle, sampler=sampler, num_workers=0)


def epoch(model, batches, device, optimizer=None, collect=False):
    training = optimizer is not None
    model.train(training)
    loss_sum = weight_sum = correct = 0.0
    probabilities, hard_targets = [], []
    for values, targets, weights in batches:
        values, targets, weights = values.to(device), targets.to(device), weights.to(device)
        with torch.set_grad_enabled(training):
            logits = model(values)
            losses = nn.functional.binary_cross_entropy_with_logits(logits, targets, reduction="none")
            loss = (losses * weights).sum() / weights.sum().clamp_min(1e-6)
            if training:
                optimizer.zero_grad(set_to_none=True); loss.backward(); optimizer.step()
        loss_sum += float((losses * weights).sum().detach().cpu())
        weight_sum += float(weights.sum().cpu())
        correct += float(((logits >= 0) == (targets >= 0.5)).sum().cpu())
        if collect:
            probabilities.append(logits.sigmoid().detach().cpu().numpy())
            hard_targets.append((targets >= 0.5).detach().cpu().numpy().astype(np.int8))
    collected = (np.concatenate(probabilities), np.concatenate(hard_targets)) if collect else (None, None)
    return loss_sum / max(weight_sum, 1), correct / len(batches.dataset), *collected


def fit(model, train, validation, device, epochs, patience, rate):
    optimizer = torch.optim.AdamW(model.parameters(), lr=rate, weight_decay=1e-4)
    best = -1.0; stale = 0; state = None
    for number in range(1, epochs + 1):
        train_loss, train_accuracy, _, _ = epoch(model, train, device, optimizer)
        val_loss, val_accuracy, probability, target = epoch(model, validation, device, collect=True)
        validation_ap = float(average_precision_score(target, probability))
        sources = validation.dataset.sources[validation.dataset.indices]
        domain_ap = []
        for source in np.unique(sources):
            selected = sources == source
            if len(np.unique(target[selected])) == 2:
                domain_ap.append(float(average_precision_score(target[selected], probability[selected])))
        worst_ap = min(domain_ap) if domain_ap else validation_ap
        selection_score = 0.5 * validation_ap + 0.5 * worst_ap
        print(f"epoch={number:03d} train_loss={train_loss:.4f} train_acc={train_accuracy:.4f} "
              f"val_loss={val_loss:.4f} val_acc={val_accuracy:.4f} val_ap={validation_ap:.4f} "
              f"worst_domain_ap={worst_ap:.4f}", flush=True)
        if selection_score > best + 1e-4:
            best, stale = selection_score, 0
            state = {key: value.detach().cpu().clone() for key, value in model.state_dict().items()}
        else:
            stale += 1
            if stale >= patience: break
    model.load_state_dict(state)


@torch.no_grad()
def predict(model, batches, device):
    model.eval(); result=[]; targets=[]
    for values, target, _ in batches:
        result.append(model(values.to(device)).sigmoid().cpu().numpy()); targets.append(target.numpy())
    return np.concatenate(result), np.concatenate(targets)


def metrics(target, probability):
    precision, recall, thresholds = precision_recall_curve(target.astype(np.int32), probability)
    f1 = 2 * precision * recall / np.maximum(precision + recall, 1e-9)
    best = int(np.nanargmax(f1[:-1])) if len(thresholds) else 0
    threshold = float(thresholds[best]) if len(thresholds) else 0.5
    selected = probability >= threshold
    tp = int(((target == 1) & selected).sum()); fp = int(((target == 0) & selected).sum())
    fn = int(((target == 1) & ~selected).sum()); tn = int(((target == 0) & ~selected).sum())
    return {"threshold": threshold, "precision": float(precision[best]), "recall": float(recall[best]),
            "f1": float(f1[best]), "tp": tp, "fp": fp, "fn": fn, "tn": tn}


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--features",type=Path,required=True); parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--teacher-epochs",type=int,default=50); parser.add_argument("--student-epochs",type=int,default=60)
    parser.add_argument("--batch",type=int,default=32)
    parser.add_argument("--student", choices=("tiny", "medium"), default="medium")
    args=parser.parse_args()
    random.seed(SEED); np.random.seed(SEED); torch.manual_seed(SEED)
    device=torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    metadata=json.loads((args.features/"metadata.json").read_text()); snore_id=metadata["labels"].index("snore")
    splits=np.load(args.features/"splits.npy"); sources=np.load(args.features/"sources.npy")
    indices={name:np.flatnonzero(splits==name) for name in ("train","validation","test")}
    # This one-second corpus performed below chance-adjusted usefulness in the
    # held-out source audit. Keep it as a foreign-domain test, not training data.
    indices["train"] = indices["train"][sources[indices["train"]] != "kaggle"]
    high=int(np.searchsorted(metadata["mel_frequencies"],2000,side="right"))
    datasets={name:BinaryDataset(args.features,index,high,name=="train",snore_id=snore_id) for name,index in indices.items()}
    print(f"device={device} train={len(indices['train'])} validation={len(indices['validation'])} test={len(indices['test'])}")
    teacher=BinaryTeacher().to(device)
    fit(teacher,make_loader(datasets["train"],args.batch,balanced=True),make_loader(datasets["validation"],args.batch),device,args.teacher_epochs,10,7e-4)
    teacher_probability,_=predict(teacher,make_loader(datasets["train"],args.batch),device)
    hard=(datasets["train"].labels[datasets["train"].indices]==snore_id).astype(np.float32)
    soft=0.70*hard+0.30*teacher_probability
    student_train=BinaryDataset(args.features,indices["train"],high,True,snore_id=snore_id,soft_targets=soft)
    student_class = BinaryStudentMedium if args.student == "medium" else BinaryStudent
    student=student_class().to(device)
    fit(student,make_loader(student_train,args.batch,balanced=True),make_loader(datasets["validation"],args.batch),device,args.student_epochs,12,8e-4)
    validation_probability,validation_target=predict(student,make_loader(datasets["validation"],args.batch),device)
    test_probability,test_target=predict(student,make_loader(datasets["test"],args.batch),device)
    selected=metrics(validation_target,validation_probability); test=metrics(test_target,test_probability)
    # Also report test performance at the validation-selected operating point.
    threshold=selected["threshold"]; decision=test_probability>=threshold
    tp=int(((test_target==1)&decision).sum()); fp=int(((test_target==0)&decision).sum()); fn=int(((test_target==1)&~decision).sum())
    deployed={"threshold":threshold,"precision":tp/max(tp+fp,1),"recall":tp/max(tp+fn,1),
              "f1":2*tp/max(2*tp+fp+fn,1),"tp":tp,"fp":fp,"fn":fn,"tn":int(((test_target==0)&~decision).sum())}
    args.output.mkdir(parents=True,exist_ok=True); torch.save(teacher.cpu().state_dict(),args.output/"teacher_binary.pt"); torch.save(student.cpu().state_dict(),args.output/"student_binary.pt")
    report={"validation":selected,"test_at_validation_threshold":deployed,"test_oracle_diagnostic":test,"firmware_eligible":False,"reason":"event and independent board gates not evaluated"}
    (args.output/"evaluation_binary.json").write_text(json.dumps(report,indent=2)+"\n")
    (args.output/"model_metadata_binary.json").write_text(json.dumps({**metadata,**selected,
                                                                       "architecture": args.student},indent=2)+"\n")
    shape=np.load(args.features/"features.npy",mmap_mode="r").shape
    torch.onnx.export(student,torch.zeros(1,2,shape[1],shape[2]),args.output/"sleep_ai_binary.onnx",input_names=["logmel_and_mask"],output_names=["snore_logit"],opset_version=18,dynamo=False,dynamic_axes={"logmel_and_mask":{0:"batch"},"snore_logit":{0:"batch"}})
    print(json.dumps(report,indent=2))


if __name__=="__main__": main()
