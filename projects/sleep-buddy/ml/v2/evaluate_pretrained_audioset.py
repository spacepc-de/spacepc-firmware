#!/usr/bin/env python3
"""Evaluate an unmodified EfficientAT AudioSet checkpoint on Board WAV files."""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
import torch


def load_mic2(path: Path, rate: int) -> np.ndarray:
    audio, source_rate = sf.read(path, dtype="float32", always_2d=True)
    channel = audio[:, 1 if audio.shape[1] > 1 else 0]
    if source_rate != rate:
        channel = librosa.resample(channel, orig_sr=source_rate, target_sr=rate)
    return channel.astype(np.float32)


def windows(audio: np.ndarray, samples: int, hop: int) -> np.ndarray:
    if len(audio) < samples:
        return np.pad(audio, (0, samples - len(audio)))[None]
    starts = np.arange(0, len(audio) - samples + 1, hop)
    return np.stack([audio[start:start + samples] for start in starts])


@torch.no_grad()
def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--efficientat-root", type=Path, required=True)
    parser.add_argument("--recordings", type=Path, required=True)
    parser.add_argument("--model", default="dymn04_as")
    parser.add_argument("--window", type=float, default=2.0)
    parser.add_argument("--hop", type=float, default=0.5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    sys.path.insert(0, str(args.efficientat_root))
    os.chdir(args.efficientat_root)
    from helpers.utils import NAME_TO_WIDTH, labels  # noqa: PLC0415
    from models.dymn.model import get_model as get_dymn  # noqa: PLC0415
    from models.mn.model import get_model as get_mobilenet  # noqa: PLC0415
    from models.preprocess import AugmentMelSTFT  # noqa: PLC0415

    rate = 32000
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    factory = get_dymn if args.model.startswith("dymn") else get_mobilenet
    model = factory(width_mult=NAME_TO_WIDTH(args.model), pretrained_name=args.model,
                    strides=[2, 2, 2, 2]).to(device).eval()
    mel = AugmentMelSTFT(n_mels=128, sr=rate, win_length=800, hopsize=320).to(device).eval()
    snoring_index = labels.index("Snoring")
    selected_labels = ("Snoring", "Breathing", "Snort", "Speech", "Air conditioning")
    selected_indices = {label: labels.index(label) for label in selected_labels if label in labels}

    report: dict[str, object] = {
        "checkpoint": args.model, "device": str(device), "window_s": args.window,
        "hop_s": args.hop, "channel": "Mic 2", "recordings": {},
        "note": "Generic AudioSet checkpoint; no project data used for training.",
    }
    for path in sorted(args.recordings.glob("*.wav")):
        batch = windows(load_mic2(path, rate), int(args.window * rate), int(args.hop * rate))
        values = []
        selected = {label: [] for label in selected_indices}
        for first in range(0, len(batch), 16):
            waveform = torch.from_numpy(batch[first:first + 16]).to(device)
            spectrum = mel(waveform)
            logits, _ = model(spectrum.unsqueeze(1))
            probability = logits.float().sigmoid().cpu().numpy()
            values.extend(probability[:, snoring_index].tolist())
            for label, index in selected_indices.items():
                selected[label].extend(probability[:, index].tolist())
        values_array = np.asarray(values)
        report["recordings"][path.name] = {
            "windows": len(values), "snoring_mean": float(values_array.mean()),
            "snoring_median": float(np.median(values_array)),
            "snoring_p90": float(np.quantile(values_array, 0.9)),
            "snoring_max": float(values_array.max()),
            "above_0_5": int((values_array >= 0.5).sum()),
            "selected_label_mean": {label: float(np.mean(probability))
                                    for label, probability in selected.items()},
        }
        print(path.name, json.dumps(report["recordings"][path.name]))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
