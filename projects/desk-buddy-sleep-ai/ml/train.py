#!/usr/bin/env python3
"""Train a quantized snore/not-snore Log-Mel CNN from licensed WAV files."""
import argparse
import csv
from pathlib import Path
import librosa
import numpy as np
import tensorflow as tf
from sklearn.model_selection import train_test_split

RATE, SAMPLES, MELS = 16_000, 16_000, 40

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
    mel = librosa.feature.melspectrogram(y=audio, sr=RATE, n_fft=512, hop_length=160,
        win_length=400, n_mels=MELS, fmin=40, fmax=7_800)
    return ((librosa.power_to_db(mel, ref=np.max) + 80) / 80).clip(0, 1).astype(np.float32)[..., None]

def dataset(root, sources):
    x, y = [], []
    for source in sorted(sources):
        for name, target in (("not_snore", 0), ("snore", 1)):
            for wav in sorted((root / source / name).glob("*.wav")):
                x.append(feature(wav)); y.append(target)
    if len(x) < 40 or len(set(y)) != 2:
        raise SystemExit("Need at least 40 licensed WAV clips containing both classes")
    return np.stack(x), np.asarray(y, dtype=np.float32)

def build_model(shape):
    inputs = tf.keras.Input(shape=shape); x = inputs
    for channels in (12, 20, 28):
        x = tf.keras.layers.SeparableConv2D(channels, 3, padding="same", activation="relu")(x)
        x = tf.keras.layers.BatchNormalization()(x); x = tf.keras.layers.MaxPool2D(2)(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x); x = tf.keras.layers.Dropout(.2)(x)
    return tf.keras.Model(inputs, tf.keras.layers.Dense(1, activation="sigmoid")(x), name="snore_v1")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/snore_v1_int8.tflite"))
    parser.add_argument("--epochs", type=int, default=40)
    args = parser.parse_args()
    x, y = dataset(args.dataset, licensed_sources(args.dataset / "LICENSES.csv"))
    xt, xv, yt, yv = train_test_split(x, y, test_size=.2, stratify=y, random_state=42)
    model = build_model(x.shape[1:])
    model.compile(optimizer="adam", loss="binary_crossentropy", metrics=["accuracy", tf.keras.metrics.AUC(name="auc")])
    model.fit(xt, yt, validation_split=.2, epochs=args.epochs, batch_size=16,
        callbacks=[tf.keras.callbacks.EarlyStopping(patience=7, restore_best_weights=True)])
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
