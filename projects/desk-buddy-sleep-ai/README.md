# Desk Buddy Sleep AI

Local-first snore detection research for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.
This is not a medical device and does not diagnose sleep apnea or sleep disorders.

## Current state

The validated hardware recorder captures both onboard ES7210 microphones as 16 kHz, 16-bit stereo
PCM and writes finalized WAV files to a FAT32 microSD card. The application shell includes Monitor,
Morning Summary, History, Dataset Recorder and persistent Settings screens. Runtime modules implement
event hysteresis, session aggregation, an explicitly non-medical acoustic score and JSONL storage.

The firmware deliberately reports **No model installed**. No public dataset with unclear recording
rights is copied into the repository and a sound-level heuristic is never presented as AI.

Insert a FAT32 microSD card before booting. Open **Recorder**, tap **Start recording**, generate the
test audio, then tap **Stop & save** before removing the card. Files are named `/ST0001.WAV` through
`/ST9999.WAV`; Mic 1 is left and Mic 2 is right. Input gain is 24 dB.

## Train Snore AI v1

`ml/train.py` creates one-second Log-Mel features, trains a small depthwise-separable CNN, evaluates a
held-out stratified test split and exports a fully int8 TFLite model. It refuses to train unless every
included dataset source has a known license in `LICENSES.csv`.

```text
dataset/
  LICENSES.csv
  own_device_recordings/
    snore/*.wav
    not_snore/*.wav
```

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r ml/requirements.txt
cp ml/LICENSES.example.csv /path/to/dataset/LICENSES.csv
python ml/train.py --dataset /path/to/dataset
```

The model is not automatically committed. Before enabling firmware inference it must pass hardware
validation for false positives per hour, recall, latency and memory, then be integrated through the
selected ESP32-P4 inference runtime. A public Edge Impulse snoring example is useful as a baseline,
but its underlying recordings must not be redistributed without confirmed rights.

## Acoustic score

The 0-100 acoustic score is a descriptive summary derived from detected percentage, events per hour
and longest event. It is not a sleep-quality score and has no diagnostic meaning.

## Build

Use ESP-IDF 5.5:

```sh
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

## Privacy

Recordings and result files remain on the inserted microSD card. Delete or retain recordings according
to the consent of everyone who could be recorded. Event-audio retention is intended to remain opt-in.
