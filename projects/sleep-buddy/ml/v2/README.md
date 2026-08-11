# Sleep AI training pipeline v2

This pipeline trains and validates a multi-domain snore event detector. Raw
audio and generated features stay outside Git.

## Release gates

A model is eligible for firmware integration only when an unseen-group test
set reaches all of these thresholds:

- event recall >= 0.90
- event precision >= 0.85
- event F1 >= 0.87
- false positive events per hour <= 0.20
- board breathing recall >= 0.90

Clip accuracy is reported for diagnostics but is not a release gate.

## Data model

`build_manifest.py` emits one row per source interval. Every row carries a
person/night/source group. `prepare_features.py` assigns complete groups to
train, validation, or test before extracting overlapping two-second examples.
Consequently, fragments from one recording can never leak between splits.

Canonical classes are:

1. `snore`
2. `breathing`
3. `human_noise` (cough, sneeze, throat clearing)
4. `speech`
5. `movement`
6. `environment`
7. `silence`

The teacher uses a two-branch log-Mel representation. A 0-2 kHz branch can be
trained by every source, including 4 kHz APSAA audio. A 2-8 kHz branch is
masked for limited-bandwidth recordings. Random high-band dropout prevents
the dataset bandwidth from becoming a class shortcut.

The student is a small depthwise-separable CNN and is exported as fully INT8
TFLite. Firmware integration is deliberately a separate gated step.

## Example

```sh
python build_manifest.py \
  --apsaa /datasets/APSAA \
  --kaggle-snoring "/datasets/Snoring Dataset" \
  --board /datasets/board \
  --psg /datasets/psg-audio \
  --coswara /datasets/coswara-selected \
  --output /work/v2/manifest.csv

python prepare_features.py \
  --manifest /work/v2/manifest.csv \
  --output /work/v2/features

python train_torch.py \
  --features /work/v2/features \
  --output /work/v2/model

python train_binary.py \
  --features /work/v2/features \
  --output /work/v2/model-binary

python evaluate_board_binary.py \
  --recordings /datasets/board \
  --model /work/v2/model-binary/student_binary.pt \
  --metadata /work/v2/model-binary/model_metadata_binary.json \
  --output /work/v2/model-binary/board_evaluation.json
```

On Apple Silicon, the trainer uses Metal (`mps`) when available and otherwise
falls back to the CPU. It keeps the feature arrays memory-mapped so training
remains practical on an 8 GB machine. PyTorch 2.13 or newer is required for the
macOS 27 version string used by current Apple Silicon systems.
