# Desk Buddy Sleep AI

Local-first snore detection research for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.
This is not a medical device and does not diagnose sleep apnea or sleep disorders.

## Current state

The validated hardware recorder captures both onboard ES7210 microphones as 32 kHz, 16-bit stereo
PCM and writes finalized WAV files to a FAT32 microSD card. The application shell includes Monitor,
Morning Summary, History, Dataset Recorder and persistent Settings screens. Runtime modules implement
event hysteresis, session aggregation, an explicitly non-medical acoustic score and JSONL storage.
Settings include persistent conservative/sensitive detection profiles and an opt-in switch that starts
a finalized WAV recording together with sleep monitoring.
The Monitor screen also exposes both actions directly: **Start** performs live analysis without saving
audio, while **Start + Record** additionally writes a WAV file. A microSD card is required only for the
recording action; microphone capture and live analysis can initialize without it.

The firmware now embeds one fully-int8 **EfficientAT AudioSet MN04 Snoring** classifier. The original
527-class AudioSet output head is reduced to the unchanged pretrained `Snoring` logit; the device UI,
event detector and storage expose only snoring probability and snoring events. Balanced and Sensitive
are threshold/hysteresis profiles for this one classifier, not separate semantic models.

Firmware and desktop evaluation share the exact EfficientAT frontend: Mic 2, 32 kHz, two seconds,
pre-emphasis, 1024-point STFT, 128 Kaldi Mel bands and fixed normalization. A byte-level frontend test
produces identical INT8 input for all 25,600 feature values. The 965 kB model uses about 392 kB of the
TFLite Micro tensor arena on the ESP32-P4. No public dataset audio is copied into the repository and a
sound-level heuristic is never presented as AI.

The live classifier runs at a 1.25-second window stride. Its low-priority inference task yields after
every prediction so LVGL and the display driver remain responsive. A 1.536-second audio queue absorbs
the measured 1.07-1.14-second inference latency; the flashed runtime test completed with zero dropped
audio blocks and no task-watchdog reset. This scheduling fix does not alter model weights, DSP,
quantization or detection thresholds.

Insert a FAT32 microSD card before booting. Open **Recorder**, tap **Start recording**, generate the
test audio, then tap **Stop & save** before removing the card. Files are named `/ST0001.WAV` through
`/ST9999.WAV`; Mic 1 is left and Mic 2 is right. Input gain can be set from 24 to 36 dB.

## Training and validation

The original `ml/train.py` and V3 paths remain reproducible baselines. The current device candidate is
the MIT-licensed EfficientAT `mn04_as` AudioSet checkpoint reduced to class 43 (`Snoring`). Export,
quantization, board evaluation and sparse firmware DSP table generation live in `ml/v2/`; the exact
runtime state and remaining release gate are described below.

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

The generated model is kept out of Git and embedded from `local_models/` during a local build. The
current candidate has passed conversion parity and short Waveshare recordings, but still requires a
blind complete-night test for false positives per hour and event recall before it can be called a
release model.

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
