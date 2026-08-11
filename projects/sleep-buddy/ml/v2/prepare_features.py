#!/usr/bin/env python3
"""Create leak-free two-second log-Mel examples from an interval manifest."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import multiprocessing as mp
from pathlib import Path

import librosa
import numpy as np
import pyedflib
import soundfile as sf


RATE = 16000
DURATION = 2.0
SAMPLES = int(RATE * DURATION)
N_FFT, WIN_LENGTH, HOP_LENGTH, MEL_BANDS = 512, 400, 320, 64
FMIN, FMAX = 80, 8000
MAX_WINDOWS_PER_INTERVAL = 6
BACKGROUND_WINDOWS_PER_NIGHT = 300
SEED = 260810
LABEL_ALIASES = {"sleep_background": "environment", "not_snore": "environment"}


def stable_fraction(value: str) -> float:
    digest = hashlib.sha256(value.encode()).digest()
    return int.from_bytes(digest[:8], "big") / 2**64


def split_for(group: str) -> str:
    if group.startswith("board:"):
        return "board_test"
    value = stable_fraction(group)
    return "train" if value < 0.70 else "validation" if value < 0.85 else "test"


def exclusions(value: str) -> list[tuple[float, float]]:
    if not value:
        return []
    result = []
    for item in value.split(";"):
        start, duration = map(float, item.split(":"))
        result.append((start, start + duration))
    return result


def overlaps(start: float, intervals: list[tuple[float, float]], margin: float = 2.0) -> bool:
    end = start + DURATION
    return any(start < right + margin and end > left - margin for left, right in intervals)


def offsets(row: dict[str, str], rng: np.random.Generator) -> list[float]:
    start, duration = float(row["start_s"]), float(row["duration_s"])
    if row["label"] == "sleep_background":
        blocked = exclusions(row.get("exclude", ""))
        candidates = []
        attempts = 0
        while len(candidates) < BACKGROUND_WINDOWS_PER_NIGHT and attempts < 100000:
            candidate = float(rng.uniform(start, max(start, start + duration - DURATION)))
            if not overlaps(candidate, blocked):
                candidates.append(candidate)
            attempts += 1
        return sorted(candidates)
    candidates = np.arange(start, max(start + 0.001, start + duration - DURATION + 0.001), 0.5)
    if not len(candidates):
        return [start]
    if len(candidates) > MAX_WINDOWS_PER_INTERVAL:
        indices = np.linspace(0, len(candidates) - 1, MAX_WINDOWS_PER_INTERVAL).astype(int)
        candidates = candidates[indices]
    return candidates.astype(float).tolist()


def load_window(path: Path, start: float, channel: str = "") -> np.ndarray:
    if path.suffix.lower() == ".edf":
        with pyedflib.EdfReader(str(path)) as edf:
            labels = [label.strip() for label in edf.getSignalLabels()]
            index = labels.index(channel or "Mic")
            source_rate = int(round(edf.getSampleFrequency(index)))
            source_start = max(0, int(start * source_rate))
            count = min(int(DURATION * source_rate), edf.getNSamples()[index] - source_start)
            audio = edf.readSignal(index, source_start, count).astype(np.float32)
        if source_rate != RATE:
            audio = librosa.resample(audio, orig_sr=source_rate, target_sr=RATE)
        peak = max(float(np.max(np.abs(audio))), 1e-9)
        # EDF physical units vary between studies. Preserve dynamics within a
        # night but constrain pathological scale metadata.
        if peak > 4.0:
            audio = audio / 32768.0
        return librosa.util.fix_length(audio, size=SAMPLES).astype(np.float32)
    if channel:
        info = sf.info(path)
        source_rate = info.samplerate
        source_start = max(0, int(start * source_rate))
        audio, _ = sf.read(path, start=source_start, frames=int(DURATION * source_rate),
                           dtype="float32", always_2d=True)
        channel_index = int(channel) - 1
        if channel_index < 0 or channel_index >= audio.shape[1]:
            raise ValueError(f"channel {channel} missing in {path}")
        audio = audio[:, channel_index]
        if source_rate != RATE:
            audio = librosa.resample(audio, orig_sr=source_rate, target_sr=RATE)
    else:
        audio, _ = librosa.load(path, sr=RATE, mono=True, offset=max(0.0, start), duration=DURATION)
    return librosa.util.fix_length(audio, size=SAMPLES).astype(np.float32)


def representation(audio: np.ndarray, bandwidth_hz: float) -> tuple[np.ndarray, np.ndarray]:
    mel = librosa.feature.melspectrogram(y=audio, sr=RATE, n_fft=N_FFT, win_length=WIN_LENGTH,
                                        hop_length=HOP_LENGTH, n_mels=MEL_BANDS,
                                        fmin=FMIN, fmax=FMAX, center=False, power=2.0)
    # Normalize every window against its own spectral peak. Absolute recording
    # gain differs drastically between phones, PSG systems and the Waveshare
    # codec and otherwise becomes an easy but invalid source shortcut.
    reference = max(float(np.max(mel)), 1e-10)
    logmel = librosa.power_to_db(mel, ref=reference, top_db=80.0)
    logmel = np.clip((logmel + 80.0) / 80.0, 0.0, 1.0).T
    frequencies = librosa.mel_frequencies(n_mels=MEL_BANDS, fmin=FMIN, fmax=FMAX)
    available = (frequencies <= bandwidth_hz).astype(np.float32)
    logmel[:, available == 0] = 0.0
    mask = np.broadcast_to(available, logmel.shape)
    return logmel.astype(np.float16), mask.astype(np.float16)


def harmonic_representation(audio: np.ndarray, bandwidth_hz: float) -> tuple[np.ndarray, np.ndarray]:
    """Common-band full and HPSS-harmonic log-Mel channels for domain robustness."""
    common_fmax = min(2000.0, bandwidth_hz)
    spectrum = librosa.stft(audio, n_fft=N_FFT, win_length=WIN_LENGTH,
                            hop_length=HOP_LENGTH, center=False)
    magnitude = np.abs(spectrum)
    harmonic_magnitude, _ = librosa.decompose.hpss(magnitude, margin=1.0)
    parameters = dict(sr=RATE, n_fft=N_FFT, n_mels=MEL_BANDS, fmin=FMIN, fmax=common_fmax)
    full_mel = librosa.feature.melspectrogram(S=magnitude**2, **parameters)
    harmonic_mel = librosa.feature.melspectrogram(S=harmonic_magnitude**2, **parameters)
    reference = max(float(np.max(full_mel)), 1e-10)

    def normalize(value: np.ndarray) -> np.ndarray:
        value = librosa.power_to_db(value, ref=reference, top_db=80.0)
        return np.clip((value + 80.0) / 80.0, 0.0, 1.0).T.astype(np.float16)

    return normalize(full_mel), normalize(harmonic_mel)


def process_row(payload: tuple[int, dict[str, str], str]) -> tuple[int, dict[str, list]]:
    index, row, representation_name = payload
    rng = np.random.default_rng(SEED + index)
    label = LABEL_ALIASES.get(row["label"], row["label"])
    weak = row["source"] == "apsaa"
    weight = (0.55 if row["label"] == "snore" and weak else
              0.30 if row["label"] == "sleep_background" else
              0.50 if row["source"] == "kaggle" else 1.0)
    result: dict[str, list] = {name: [] for name in
                              ("features", "masks", "labels", "weights", "groups",
                               "sources", "starts", "paths", "splits")}
    extractor = harmonic_representation if representation_name == "harmonic" else representation
    for start in offsets(row, rng):
        audio = load_window(Path(row["path"]), start, row.get("channel", ""))
        feature, mask = extractor(audio, float(row["bandwidth_hz"]))
        result["features"].append(feature); result["masks"].append(mask)
        result["labels"].append(label); result["weights"].append(weight)
        result["groups"].append(row["group"]); result["sources"].append(row["source"])
        result["starts"].append(start); result["paths"].append(row["path"])
        result["splits"].append(split_for(row["group"]))
    return index, result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--representation", choices=("masked", "harmonic"), default="masked")
    parser.add_argument("--workers", type=int, default=1)
    args = parser.parse_args()
    with args.manifest.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    labels = sorted({LABEL_ALIASES.get(row["label"], row["label"]) for row in rows})
    label_ids = {label: index for index, label in enumerate(labels)}
    examples: dict[str, list] = {name: [] for name in ("features", "masks", "labels", "weights", "groups", "sources", "starts", "paths", "splits")}
    payloads = ((index, row, args.representation) for index, row in enumerate(rows, 1))
    pool = None
    if args.workers > 1:
        pool = mp.get_context("spawn").Pool(args.workers)
        results = pool.imap(process_row, payloads, chunksize=1)
    else:
        results = map(process_row, payloads)
    for index, result in results:
        result["labels"] = [label_ids[label] for label in result["labels"]]
        for name in examples:
            examples[name].extend(result[name])
        if index % 500 == 0:
            print(f"manifest_rows={index}/{len(rows)} examples={len(examples['labels'])}")
    if pool is not None:
        pool.close(); pool.join()
    args.output.mkdir(parents=True, exist_ok=True)
    np.save(args.output / "features.npy", np.asarray(examples["features"], np.float16))
    np.save(args.output / "masks.npy", np.asarray(examples["masks"], np.float16))
    np.save(args.output / "labels.npy", np.asarray(examples["labels"], np.int16))
    np.save(args.output / "weights.npy", np.asarray(examples["weights"], np.float32))
    for name in ("groups", "sources", "paths", "splits"):
        np.save(args.output / f"{name}.npy", np.asarray(examples[name]))
    np.save(args.output / "starts.npy", np.asarray(examples["starts"], np.float32))
    effective_fmax = 2000 if args.representation == "harmonic" else FMAX
    metadata = dict(rate=RATE, duration_s=DURATION, n_fft=N_FFT, win_length=WIN_LENGTH,
                    hop_length=HOP_LENGTH, mel_bands=MEL_BANDS, fmin=FMIN, fmax=effective_fmax,
                    representation=args.representation,
                    mel_frequencies=librosa.mel_frequencies(n_mels=MEL_BANDS, fmin=FMIN, fmax=effective_fmax).tolist(),
                    labels=labels, label_ids=label_ids, examples=len(examples["labels"]))
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    unique, counts = np.unique(examples["splits"], return_counts=True)
    print(f"features={args.output} examples={len(examples['labels'])} splits={dict(zip(unique, counts))} labels={labels}")


if __name__ == "__main__":
    main()
