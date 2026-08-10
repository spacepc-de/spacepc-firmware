#!/usr/bin/env python3
"""Build a source-grouped interval manifest without copying raw audio."""
from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path
import wave
import xml.etree.ElementTree as ET

import numpy as np
import pyedflib


FIELDS = ("path", "channel", "label", "group", "source", "start_s", "duration_s", "bandwidth_hz")


def clock_seconds(value: str) -> float:
    hours, minutes, seconds = value.split(":")
    return int(hours) * 3600 + int(minutes) * 60 + float(seconds)


def wav_duration(path: Path) -> float:
    with wave.open(str(path), "rb") as wav:
        return wav.getnframes() / wav.getframerate()


def add_apsaa(root: Path, rows: list[dict[str, object]]) -> None:
    for directory in sorted(path for path in root.iterdir() if path.is_dir()):
        subject = directory.name
        audio = directory / f"{subject}.wav"
        annotations = directory / f"{subject}_Annotations.csv"
        if not audio.exists() or not annotations.exists():
            continue
        snore: list[tuple[float, float]] = []
        with annotations.open(newline="", encoding="utf-8-sig") as stream:
            for annotation in csv.DictReader(stream):
                if annotation["Event_Name"] == "Snore":
                    start = clock_seconds(annotation["Start_Time"])
                    snore.append((start, float(annotation["Duration"])))
        envelope_path = directory / f"{subject}_Snore_EG.csv"
        envelope = np.loadtxt(envelope_path, skiprows=1, dtype=np.float32)
        envelope = envelope[:len(envelope) // 10 * 10].reshape(-1, 10)
        envelope = np.max(np.abs(envelope), axis=1)
        annotated = np.zeros(len(envelope), dtype=bool)
        for start, duration in snore:
            annotated[max(0, int(start)):min(len(annotated), int(np.ceil(start + duration)))] = True
        inside, outside = envelope[annotated], envelope[~annotated]
        threshold = max(float(np.percentile(inside, 75)), float(np.percentile(outside, 95)) * 2.0)
        peaks = np.flatnonzero(annotated & (envelope >= threshold))
        # Avoid hundreds of near-identical consecutive windows and keep each
        # person equally represented.
        if len(peaks) > 300:
            peaks = peaks[np.linspace(0, len(peaks) - 1, 300).astype(int)]
        for second in peaks:
            rows.append(dict(path=audio, label="snore", group=f"apsaa:{subject}", source="apsaa",
                             start_s=max(0, float(second) - 0.5), duration_s=2.0, bandwidth_hz=2000))
        # Unannotated regions are intentionally not flattened into thousands of
        # rows. Feature preparation samples them while excluding snore margins.
        encoded = ";".join(f"{start:.3f}:{duration:.3f}" for start, duration in snore)
        rows.append(dict(path=audio, label="sleep_background", group=f"apsaa:{subject}", source="apsaa",
                         start_s=0, duration_s=wav_duration(audio), bandwidth_hz=2000,
                         exclude=encoded))


def add_kaggle(root: Path, rows: list[dict[str, object]]) -> None:
    seen: set[str] = set()
    for directory, label in (("1", "snore"), ("0", "environment")):
        for path in sorted((root / directory).glob("*.wav")):
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            index = int(path.stem.split("_")[-1])
            rows.append(dict(path=path, label=label, group=f"kaggle:{index // 50}", source="kaggle",
                             start_s=0, duration_s=wav_duration(path), bandwidth_hz=22050))


def add_board(root: Path, rows: list[dict[str, object]]) -> None:
    mapping = (("snore-board-001.wav", "snore"), ("breathing-board-001.wav", "breathing"))
    for filename, label in mapping:
        path = root / filename
        if path.exists():
            rows.append(dict(path=path, channel="2", label=label, group="board:jonathan:test", source="board",
                             start_s=0, duration_s=wav_duration(path), bandwidth_hz=24000))


def add_audiofolder(root: Path, source: str, rows: list[dict[str, object]]) -> None:
    """Import `root/<label>/<group>/*.wav` corpora such as Coswara exports."""
    for label_dir in sorted(path for path in root.iterdir() if path.is_dir()):
        for path in sorted(label_dir.rglob("*.wav")):
            relative = path.relative_to(label_dir)
            group = relative.parts[0] if len(relative.parts) > 1 else path.stem
            rows.append(dict(path=path, label=label_dir.name, group=f"{source}:{group}", source=source,
                             start_s=0, duration_s=wav_duration(path), bandwidth_hz=8000))


def add_coswara(root: Path, rows: list[dict[str, object]]) -> None:
    """Import selected Coswara respiratory and voice recordings without copying them."""
    labels = {
        "breathing-deep.wav": "breathing",
        "breathing-shallow.wav": "breathing",
        "cough-heavy.wav": "human_noise",
        "cough-shallow.wav": "human_noise",
        "counting-fast.wav": "speech",
        "counting-normal.wav": "speech",
        "vowel-a.wav": "speech",
        "vowel-e.wav": "speech",
        "vowel-o.wav": "speech",
    }
    for path in sorted(root.rglob("*.wav")):
        label = labels.get(path.name)
        if label is None:
            continue
        # Participant IDs are stable across collection dates. Keeping them as
        # the group prevents recordings from one person leaking across splits.
        participant = path.parent.name
        rows.append(dict(path=path, label=label, group=f"coswara:{participant}", source="coswara",
                         start_s=0, duration_s=wav_duration(path), bandwidth_hz=8000))


def add_psg(root: Path, rows: list[dict[str, object]]) -> None:
    """Import Hugging Face PSG-Audio EDF chunks and matching clean RML."""
    edf_root = root / "V3" / "APNEA_EDF"
    rml_root = root / "V1" / "APNEA_RML_clean"
    for subject_dir in sorted(path for path in edf_root.iterdir() if path.is_dir()):
        subject = subject_dir.name
        rml = rml_root / f"{subject}.rml"
        edfs = sorted(subject_dir.glob("*.edf"))
        if not rml.exists() or not edfs:
            continue
        xml_root = ET.parse(rml).getroot()
        events = []
        for event in xml_root.iter():
            if event.tag.endswith("Event") and event.attrib.get("Family") == "Nasal" and event.attrib.get("Type") == "Snore":
                events.append((float(event.attrib["Start"]), float(event.attrib["Duration"])))
        cumulative = 0.0
        for edf_path in edfs:
            with pyedflib.EdfReader(str(edf_path)) as edf:
                duration = float(edf.file_duration)
                labels = [label.strip() for label in edf.getSignalLabels()]
                if "Mic" not in labels or edf.getSampleFrequency(labels.index("Mic")) < 16000:
                    raise RuntimeError(f"48 kHz Mic channel missing in {edf_path}")
            local_events = []
            for start, event_duration in events:
                if cumulative <= start < cumulative + duration:
                    local = start - cumulative
                    clipped = min(event_duration, duration - local)
                    local_events.append((local, clipped))
                    rows.append(dict(path=edf_path, channel="Mic", label="snore", group=f"psg:{subject}", source="psg",
                                     start_s=local, duration_s=clipped, bandwidth_hz=24000))
            encoded = ";".join(f"{start:.3f}:{length:.3f}" for start, length in local_events)
            rows.append(dict(path=edf_path, channel="Mic", label="sleep_background", group=f"psg:{subject}", source="psg",
                             start_s=0, duration_s=duration, bandwidth_hz=24000, exclude=encoded))
            cumulative += duration


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apsaa", type=Path)
    parser.add_argument("--kaggle-snoring", type=Path)
    parser.add_argument("--board", type=Path)
    parser.add_argument("--psg", type=Path)
    parser.add_argument("--coswara", type=Path)
    parser.add_argument("--audiofolder", action="append", default=[], metavar="SOURCE=PATH")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows: list[dict[str, object]] = []
    if args.apsaa: add_apsaa(args.apsaa, rows)
    if args.kaggle_snoring: add_kaggle(args.kaggle_snoring, rows)
    if args.board: add_board(args.board, rows)
    if args.psg: add_psg(args.psg, rows)
    if args.coswara: add_coswara(args.coswara, rows)
    for value in args.audiofolder:
        source, path = value.split("=", 1)
        add_audiofolder(Path(path), source, rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fields = FIELDS + ("exclude",)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    print(f"manifest={args.output} rows={len(rows)} groups={len({row['group'] for row in rows})}")


if __name__ == "__main__":
    main()
