#!/usr/bin/env python3
"""Evaluate a candidate on continuous, unseen PSG nights at event level."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np
import tensorflow as tf

from prepare_features import representation, load_window, split_for


HOP = 0.5


def merge(intervals: list[tuple[float, float]], gap: float = 2.0) -> list[tuple[float, float]]:
    result: list[list[float]] = []
    for start, end in sorted(intervals):
        if result and start <= result[-1][1] + gap:
            result[-1][1] = max(result[-1][1], end)
        else:
            result.append([start, end])
    return [(start, end) for start, end in result]


def detect(probabilities: list[float], start_threshold: float, end_threshold: float) -> list[tuple[float, float]]:
    events, active_start = [], None
    above, below = 0, 0
    for index, probability in enumerate(probabilities):
        timestamp = index * HOP
        if active_start is None:
            above = above + 1 if probability >= start_threshold else 0
            if above >= 2:
                active_start = max(0.0, timestamp - HOP)
                below = 0
        else:
            below = below + 1 if probability < end_threshold else 0
            if below >= 3:
                end = timestamp - HOP
                if end - active_start >= 1.5:
                    events.append((active_start, end))
                active_start, above, below = None, 0, 0
    if active_start is not None:
        events.append((active_start, len(probabilities) * HOP))
    return merge(events)


def match(reference: list[tuple[float, float]], detected: list[tuple[float, float]]) -> tuple[int, int, int]:
    available = set(range(len(detected)))
    true_positive = 0
    for left, right in reference:
        candidates = [(max(0.0, min(right, d_right) - max(left, d_left)), index)
                      for index, (d_left, d_right) in enumerate(detected) if index in available]
        overlap, index = max(candidates, default=(0.0, -1))
        if overlap >= 0.5:
            true_positive += 1
            available.remove(index)
    return true_positive, len(available), len(reference) - true_positive


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--evaluation", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text())
    prior = json.loads(args.evaluation.read_text())
    labels = metadata["labels"]
    snore_id = labels.index("snore")
    start_threshold = float(metadata["threshold"])
    end_threshold = max(0.20, start_threshold * 0.65)
    model = tf.keras.models.load_model(args.model)
    with args.manifest.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    paths = sorted({row["path"] for row in rows if row["source"] == "psg" and split_for(row["group"]) == "test"})
    totals = dict(tp=0, fp=0, fn=0, monitored_seconds=0.0)
    nights = []
    for path_value in paths:
        path_rows = [row for row in rows if row["path"] == path_value]
        background = next(row for row in path_rows if row["label"] == "sleep_background")
        duration = float(background["duration_s"])
        reference = merge([(float(row["start_s"]), float(row["start_s"]) + float(row["duration_s"]))
                           for row in path_rows if row["label"] == "snore"])
        probabilities = []
        batch = []
        starts = np.arange(0.0, max(0.0, duration - 2.0), HOP)
        for start in starts:
            feature, mask = representation(load_window(Path(path_value), float(start), background.get("channel", "")),
                                           float(background["bandwidth_hz"]))
            batch.append(np.stack((feature, mask), axis=-1))
            if len(batch) == 64:
                probabilities.extend(model.predict(np.asarray(batch), verbose=0)[:, snore_id].tolist())
                batch = []
        if batch:
            probabilities.extend(model.predict(np.asarray(batch), verbose=0)[:, snore_id].tolist())
        detected = detect(probabilities, start_threshold, end_threshold)
        tp, fp, fn = match(reference, detected)
        totals["tp"] += tp; totals["fp"] += fp; totals["fn"] += fn; totals["monitored_seconds"] += duration
        nights.append(dict(path=path_value, reference_events=len(reference), detected_events=len(detected), tp=tp, fp=fp, fn=fn))
    precision = totals["tp"] / max(1, totals["tp"] + totals["fp"])
    recall = totals["tp"] / max(1, totals["tp"] + totals["fn"])
    f1 = 2 * precision * recall / max(1e-9, precision + recall)
    fp_per_hour = totals["fp"] / max(1e-9, totals["monitored_seconds"] / 3600)
    gates = dict(event_precision=precision >= 0.85, event_recall=recall >= 0.90,
                 event_f1=f1 >= 0.87, false_positives_per_hour=fp_per_hour <= 0.20)
    result = dict(event_precision=precision, event_recall=recall, event_f1=f1,
                  false_positives_per_hour=fp_per_hour, totals=totals, nights=nights, gates=gates)
    prior["event_evaluation"] = result
    prior["firmware_eligible"] = all(gates.values())
    prior["reason"] = "all event gates passed" if prior["firmware_eligible"] else "one or more event gates failed"
    args.output.write_text(json.dumps(prior, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
