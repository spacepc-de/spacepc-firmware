#!/usr/bin/env python3
"""Create and verify a fully-int8 EfficientAT Snoring TFLite model."""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
import tensorflow as tf
import torch

from prepare_features import load_window, split_for


RATE = 32000
SAMPLES = RATE * 2
HOP = RATE // 2
SEED = 260810


def board_windows(path: Path) -> np.ndarray:
    audio, source_rate = sf.read(path, dtype="float32", always_2d=True)
    audio = audio[:, 1 if audio.shape[1] > 1 else 0]
    if source_rate != RATE:
        audio = librosa.resample(audio, orig_sr=source_rate, target_sr=RATE)
    if len(audio) < SAMPLES:
        audio = np.pad(audio, (0, SAMPLES - len(audio)))
    starts = np.arange(0, len(audio) - SAMPLES + 1, HOP)
    return np.stack([audio[start:start + SAMPLES] for start in starts]).astype(np.float32)


def calibration_rows(path: Path, per_stratum: int) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = [row for row in csv.DictReader(stream)
                if row["source"] != "board" and split_for(row["group"]) == "train"]
    strata: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        strata[(row["source"], row["label"])].append(row)
    rng = np.random.default_rng(SEED)
    selected = []
    for key in sorted(strata):
        values = strata[key]
        indices = rng.choice(len(values), min(per_stratum, len(values)), replace=False)
        selected.extend(values[int(index)] for index in indices)
    rng.shuffle(selected)
    return selected


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--saved-model", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--efficientat-root", type=Path, required=True)
    parser.add_argument("--board-recordings", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--per-stratum", type=int, default=48)
    args = parser.parse_args()

    sys.path.insert(0, str(args.efficientat_root))
    os.chdir(args.efficientat_root)
    from models.preprocess import AugmentMelSTFT  # noqa: PLC0415

    mel = AugmentMelSTFT(n_mels=128, sr=RATE, win_length=800, hopsize=320).eval()
    rows = calibration_rows(args.manifest, args.per_stratum)
    features: list[np.ndarray] = []
    sources: list[str] = []
    with torch.no_grad():
        for index, row in enumerate(rows, 1):
            audio16 = load_window(Path(row["path"]), float(row["start_s"]), row.get("channel", ""))
            audio32 = librosa.resample(audio16, orig_sr=16000, target_sr=RATE).astype(np.float32)
            value = mel(torch.from_numpy(audio32).unsqueeze(0)).numpy()[0]
            features.append(value[..., None].astype(np.float32))
            sources.append(f"{row['source']}:{row['label']}")
            if index % 50 == 0:
                print(f"calibration_features={index}/{len(rows)}", flush=True)

    calibration = np.stack(features)

    def representative_dataset():
        for value in calibration:
            yield [value[None]]

    converter = tf.lite.TFLiteConverter.from_saved_model(str(args.saved_model))
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    model = converter.convert()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(model)

    float_interpreter = tf.lite.Interpreter(
        model_path=str(args.saved_model / "efficientat_mn04_snoring_float32.tflite"),
        experimental_delegates=[], num_threads=4)
    int8_interpreter = tf.lite.Interpreter(model_content=model, experimental_delegates=[], num_threads=4)
    float_interpreter.allocate_tensors()
    int8_interpreter.allocate_tensors()
    float_in = float_interpreter.get_input_details()[0]
    float_out = float_interpreter.get_output_details()[0]
    int8_in = int8_interpreter.get_input_details()[0]
    int8_out = int8_interpreter.get_output_details()[0]

    def predict(interpreter, input_info, output_info, batch: np.ndarray) -> np.ndarray:
        result = []
        scale, zero = input_info["quantization"]
        output_scale, output_zero = output_info["quantization"]
        for value in batch:
            data = value[None]
            if input_info["dtype"] == np.int8:
                data = np.clip(np.rint(data / scale) + zero, -128, 127).astype(np.int8)
            interpreter.set_tensor(input_info["index"], data)
            interpreter.invoke()
            output = interpreter.get_tensor(output_info["index"]).astype(np.float32)
            if output_info["dtype"] == np.int8:
                output = (output - output_zero) * output_scale
            result.append(float(output.reshape(-1)[0]))
        return np.asarray(result, np.float32)

    # Quantization parity on representative features.
    parity_indices = np.linspace(0, len(calibration) - 1, min(96, len(calibration))).astype(int)
    parity_features = calibration[parity_indices]
    float_logits = predict(float_interpreter, float_in, float_out, parity_features)
    int8_logits = predict(int8_interpreter, int8_in, int8_out, parity_features)

    recordings: dict[str, dict[str, float | int]] = {}
    with torch.no_grad():
        for path in sorted(args.board_recordings.glob("*.wav")):
            waveform = board_windows(path)
            batch_features = []
            for first in range(0, len(waveform), 16):
                value = mel(torch.from_numpy(waveform[first:first + 16])).numpy()
                batch_features.extend(item[..., None] for item in value)
            board_features = np.asarray(batch_features, np.float32)
            logits = predict(int8_interpreter, int8_in, int8_out, board_features)
            probabilities = 1.0 / (1.0 + np.exp(-logits))
            recordings[path.name] = {
                "windows": len(probabilities),
                "mean": float(probabilities.mean()),
                "median": float(np.median(probabilities)),
                "p90": float(np.quantile(probabilities, 0.9)),
                "max": float(probabilities.max()),
                "above_0_5": int(np.count_nonzero(probabilities >= 0.5)),
            }
            print(path.name, json.dumps(recordings[path.name]), flush=True)

    report = {
        "model_bytes": len(model),
        "calibration_examples": len(calibration),
        "calibration_sources": {name: sources.count(name) for name in sorted(set(sources))},
        "input_shape": int8_in["shape"].tolist(),
        "input_quantization": list(int8_in["quantization"]),
        "output_shape": int8_out["shape"].tolist(),
        "output_quantization": list(int8_out["quantization"]),
        "parity_logit_mean_abs_error": float(np.mean(np.abs(float_logits - int8_logits))),
        "parity_logit_max_abs_error": float(np.max(np.abs(float_logits - int8_logits))),
        "board_recordings": recordings,
    }
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
