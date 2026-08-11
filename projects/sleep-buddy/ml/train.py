#!/usr/bin/env python3
"""Train a quantized raw-waveform snore classifier from licensed WAV files."""
import argparse
import csv
from pathlib import Path
import librosa
import numpy as np
import tensorflow as tf
from sklearn.model_selection import StratifiedGroupKFold

RATE, SAMPLES = 16_000, 16_000
FRAMES, BANDS = 8, 24
FRAME_SAMPLES = SAMPLES // FRAMES
FREQUENCIES = np.geomspace(80.0, 6000.0, BANDS)
PHASE = 2 * np.pi * np.arange(FRAME_SAMPLES)[:, None] * FREQUENCIES[None, :] / RATE
COSINE, SINE = np.cos(PHASE), np.sin(PHASE)

def licensed_sources(manifest):
    if not manifest.exists():
        raise SystemExit(f"Missing license manifest: {manifest}")
    with manifest.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    result = {r["source"] for r in rows if r.get("license", "").strip().lower() not in {"", "unknown"}}
    if not result:
        raise SystemExit("No dataset source with a known license is declared")
    return result

def feature(path):
    audio, _ = librosa.load(path, sr=RATE, mono=True)
    audio = librosa.util.fix_length(data=audio, size=SAMPLES)
    frames = audio.reshape(FRAMES, FRAME_SAMPLES)
    energy = np.sum(frames * frames, axis=1) + 1e-9
    real, imag = frames @ COSINE, frames @ SINE
    ratio = (real * real + imag * imag) / (energy[:, None] * FRAME_SAMPLES)
    spectral = np.clip((np.log10(ratio + 1e-8) + 8) / 8, 0, 1)
    rms = np.clip((20 * np.log10(np.sqrt(energy / FRAME_SAMPLES) + 1e-6) + 80) / 80, 0, 1)
    zcr = np.mean(frames[:, 1:] * frames[:, :-1] < 0, axis=1)
    return np.concatenate((spectral, rms[:, None], zcr[:, None]), axis=1).astype(np.float32).reshape(-1)

def dataset(root, sources):
    x, y, groups = [], [], []
    for source in sorted(sources):
        for name, target in (("not_snore", 0), ("snore", 1)):
            for wav in sorted((root / source / name).glob("*.wav")):
                x.append(feature(wav)); y.append(target); groups.append(source)
    if len(x) < 40 or len(set(y)) != 2:
        raise SystemExit("Need at least 40 licensed WAV clips containing both classes")
    return np.stack(x), np.asarray(y, dtype=np.float32), np.asarray(groups)

def build_model(shape):
    inputs = tf.keras.Input(shape=shape); x = inputs
    x = tf.keras.layers.Dense(64, activation="relu")(x)
    x = tf.keras.layers.Dropout(.25)(x)
    x = tf.keras.layers.Dense(32, activation="relu")(x)
    x = tf.keras.layers.Dropout(.15)(x)
    return tf.keras.Model(inputs, tf.keras.layers.Dense(1, activation="sigmoid")(x), name="snore_v1")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/snore_v1_int8.tflite"))
    parser.add_argument("--epochs", type=int, default=40)
    args = parser.parse_args()
    x, y, groups = dataset(args.dataset, licensed_sources(args.dataset / "LICENSES.csv"))
    split = StratifiedGroupKFold(n_splits=4, shuffle=True, random_state=42)
    train_index, test_index = next(split.split(x, y, groups))
    inner = StratifiedGroupKFold(n_splits=4, shuffle=True, random_state=7)
    fit_relative, validation_relative = next(inner.split(x[train_index], y[train_index], groups[train_index]))
    fit_index = train_index[fit_relative]; validation_index = train_index[validation_relative]
    xt, yt = x[fit_index], y[fit_index]
    xval, yval = x[validation_index], y[validation_index]
    xv, yv = x[test_index], y[test_index]
    model = build_model(x.shape[1:])
    model.compile(optimizer="adam", loss="binary_crossentropy", metrics=["accuracy", tf.keras.metrics.AUC(name="auc")])
    negative = max(1, np.sum(yt == 0)); positive = max(1, np.sum(yt == 1))
    total = len(yt); class_weight = {0: total / (2 * negative), 1: total / (2 * positive)}
    model.fit(xt, yt, validation_data=(xval, yval), epochs=args.epochs, batch_size=16, class_weight=class_weight,
        callbacks=[tf.keras.callbacks.EarlyStopping(patience=7, restore_best_weights=True)], verbose=2)
    print(dict(zip(model.metrics_names, model.evaluate(xv, yv, verbose=0))))
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = lambda: ([sample[None]] for sample in xt[:100])
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = converter.inference_output_type = tf.int8
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(converter.convert())
    print(f"Wrote {args.output}")

if __name__ == "__main__":
    main()
