#!/usr/bin/env python3
"""Download verified CC0 previews and create source-grouped one-second WAV clips."""
import argparse, csv, re, urllib.request
from pathlib import Path
import librosa
import soundfile as sf

def download(page, target):
    request = urllib.request.Request(page, headers={"User-Agent": "SleepBuddyDataset/1.0"})
    html = urllib.request.urlopen(request).read().decode("utf-8", "replace")
    if "Creative Commons 0" not in html:
        raise RuntimeError(f"CC0 marker missing at {page}")
    urls = re.findall(r'https://cdn\.freesound\.org/previews/[^" ]+-hq\.mp3', html)
    if not urls:
        raise RuntimeError(f"No HQ preview at {page}")
    urllib.request.urlretrieve(urls[0], target)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sources", type=Path, default=Path(__file__).with_name("sources.csv"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raw = args.output / "raw"; raw.mkdir(parents=True, exist_ok=True)
    with args.sources.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    with (args.output / "LICENSES.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle); writer.writerow(("source", "license", "origin", "notes"))
        for row in rows:
            writer.writerow((row["source"], row["license"], row["page"], "Freesound HQ preview"))
    for row in rows:
        media = raw / f'{row["source"]}.mp3'
        if not media.exists(): download(row["page"], media)
        audio, _ = librosa.load(media, sr=16_000, mono=True)
        destination = args.output / row["source"] / row["class"]
        destination.mkdir(parents=True, exist_ok=True)
        offsets = list(range(0, len(audio) - 16_000 + 1, 8_000))
        if len(offsets) > 240:
            step = len(offsets) / 240
            offsets = [offsets[int(i * step)] for i in range(240)]
        for index, offset in enumerate(offsets):
            clip = audio[offset:offset + 16_000]
            if max(abs(clip)) < .003: continue
            sf.write(destination / f"{index:05d}.wav", clip, 16_000, subtype="PCM_16")
    print(f"Prepared {args.output}")

if __name__ == "__main__": main()
