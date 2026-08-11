#!/usr/bin/env python3
"""Train a multi-class teacher and distill a fully INT8 embedded student."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf
from sklearn.metrics import classification_report, confusion_matrix, precision_recall_curve


SEED = 260810
BATCH = 48


def load(root: Path):
    metadata = json.loads((root / "metadata.json").read_text())
    feature = np.load(root / "features.npy", mmap_mode="r")
    mask = np.load(root / "masks.npy", mmap_mode="r")
    x = np.stack((feature, mask), axis=-1).astype(np.float32)
    return (x, np.load(root / "labels.npy").astype(np.int32),
            np.load(root / "weights.npy").astype(np.float32),
            np.load(root / "splits.npy"), metadata)


def teacher_model(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape, name="logmel_and_mask")
    x = tf.keras.layers.Conv2D(32, (5, 5), strides=(2, 2), padding="same", activation="swish")(inputs)
    for filters in (48, 64, 96):
        x = tf.keras.layers.SeparableConv2D(filters, 3, padding="same", activation="swish")(x)
        x = tf.keras.layers.BatchNormalization()(x)
        x = tf.keras.layers.MaxPool2D(2, padding="same")(x)
    avg = tf.keras.layers.GlobalAveragePooling2D()(x)
    maximum = tf.keras.layers.GlobalMaxPooling2D()(x)
    x = tf.keras.layers.Concatenate()((avg, maximum))
    x = tf.keras.layers.Dense(128, activation="swish")(x)
    x = tf.keras.layers.Dropout(0.30)(x)
    return tf.keras.Model(inputs, tf.keras.layers.Dense(classes, activation="softmax")(x), name="sleep_ai_teacher")


def student_model(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape, name="logmel_and_mask")
    x = tf.keras.layers.Conv2D(12, (5, 3), strides=(2, 2), padding="same", activation="relu")(inputs)
    for filters, stride in ((20, 2), (28, 2), (40, 1)):
        x = tf.keras.layers.DepthwiseConv2D(3, strides=stride, padding="same", activation="relu")(x)
        x = tf.keras.layers.Conv2D(filters, 1, activation="relu")(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dense(32, activation="relu")(x)
    return tf.keras.Model(inputs, tf.keras.layers.Dense(classes, activation="softmax", name="probabilities")(x),
                          name="sleep_ai_student")


def augmented_dataset(x, y, weights, high_start: int, training: bool) -> tf.data.Dataset:
    dataset = tf.data.Dataset.from_tensor_slices((x, y, weights))
    if training:
        dataset = dataset.shuffle(min(len(y), 20000), seed=SEED, reshuffle_each_iteration=True)

        def band_dropout(features, label, weight):
            drop = tf.random.uniform((), seed=SEED) < 0.35
            low = features[:, :high_start, :]
            high = tf.cond(drop, lambda: tf.zeros_like(features[:, high_start:, :]),
                           lambda: features[:, high_start:, :])
            return tf.concat((low, high), axis=1), label, weight

        dataset = dataset.map(band_dropout, num_parallel_calls=tf.data.AUTOTUNE)
    return dataset.batch(BATCH).prefetch(tf.data.AUTOTUNE)


def snore_threshold(y_true: np.ndarray, probability: np.ndarray, snore_id: int) -> dict[str, float]:
    binary = (y_true == snore_id).astype(np.int32)
    precision, recall, thresholds = precision_recall_curve(binary, probability)
    f1 = 2 * precision * recall / np.maximum(precision + recall, 1e-9)
    index = int(np.nanargmax(f1[:-1])) if len(thresholds) else 0
    return dict(threshold=float(thresholds[index]), precision=float(precision[index]),
                recall=float(recall[index]), f1=float(f1[index]))


def quantize(model: tf.keras.Model, representative: np.ndarray, output: Path) -> None:
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = lambda: ([sample[None]] for sample in representative[:500])
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    output.write_bytes(converter.convert())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--features", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--teacher-epochs", type=int, default=60)
    parser.add_argument("--student-epochs", type=int, default=80)
    args = parser.parse_args()
    tf.keras.utils.set_random_seed(SEED)
    x, y, weights, splits, metadata = load(args.features)
    labels = metadata["labels"]
    classes = len(labels)
    fit, validation, test = splits == "train", splits == "validation", splits == "test"
    if min(fit.sum(), validation.sum(), test.sum()) == 0:
        raise SystemExit(f"empty split: train={fit.sum()} validation={validation.sum()} test={test.sum()}")
    high_start = int(np.searchsorted(np.asarray(metadata["mel_frequencies"]), 2000.0, side="right"))

    teacher = teacher_model(x.shape[1:], classes)
    teacher.compile(optimizer=tf.keras.optimizers.Adam(7e-4), loss="sparse_categorical_crossentropy",
                    metrics=["accuracy"])
    teacher.fit(augmented_dataset(x[fit], y[fit], weights[fit], high_start, True),
                validation_data=augmented_dataset(x[validation], y[validation], weights[validation], high_start, False),
                epochs=args.teacher_epochs, verbose=2,
                callbacks=[tf.keras.callbacks.EarlyStopping(patience=10, restore_best_weights=True)])

    teacher_soft = teacher.predict(x[fit], batch_size=BATCH, verbose=1)
    hard = tf.one_hot(y[fit], classes).numpy()
    targets = 0.65 * hard + 0.35 * teacher_soft
    student = student_model(x.shape[1:], classes)
    student.compile(optimizer=tf.keras.optimizers.Adam(8e-4), loss="categorical_crossentropy", metrics=["accuracy"])
    student.fit(x[fit], targets, sample_weight=weights[fit], validation_data=(x[validation], tf.one_hot(y[validation], classes)),
                epochs=args.student_epochs, batch_size=BATCH, verbose=2,
                callbacks=[tf.keras.callbacks.EarlyStopping(patience=12, restore_best_weights=True)])

    args.output.mkdir(parents=True, exist_ok=True)
    teacher.save(args.output / "teacher.keras")
    student.save(args.output / "student.keras")
    probability = student.predict(x[test], batch_size=BATCH, verbose=0)
    predicted = np.argmax(probability, axis=1)
    report = classification_report(y[test], predicted, labels=np.arange(classes), target_names=labels,
                                   output_dict=True, zero_division=0)
    threshold = snore_threshold(y[validation], student.predict(x[validation], batch_size=BATCH, verbose=0)[:, labels.index("snore")],
                                labels.index("snore"))
    evaluation = dict(clip_report=report, confusion_matrix=confusion_matrix(y[test], predicted, labels=np.arange(classes)).tolist(),
                      validation_snore_threshold=threshold, split_counts=dict(train=int(fit.sum()), validation=int(validation.sum()),
                                                                             test=int(test.sum())),
                      firmware_eligible=False, reason="event-level release gates not evaluated")
    (args.output / "evaluation.json").write_text(json.dumps(evaluation, indent=2) + "\n", encoding="utf-8")
    (args.output / "model_metadata.json").write_text(json.dumps({**metadata, **threshold}, indent=2) + "\n", encoding="utf-8")
    quantize(student, x[fit], args.output / "sleep_ai_student_int8.tflite")
    print(json.dumps(evaluation, indent=2))


if __name__ == "__main__":
    main()
