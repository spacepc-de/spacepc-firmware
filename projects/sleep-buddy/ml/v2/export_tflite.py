#!/usr/bin/env python3
"""Convert a trained medium PyTorch student to a verified full-INT8 TFLite model."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf
import torch

from train_binary import BinaryStudentMedium


def conv_weights(state: dict[str, torch.Tensor], key: str) -> list[np.ndarray]:
    return [state[key].detach().cpu().numpy().transpose(2, 3, 1, 0)]


def depthwise_weights(state: dict[str, torch.Tensor], key: str) -> list[np.ndarray]:
    return [state[key].detach().cpu().numpy().transpose(2, 3, 0, 1)]


def batch_norm_weights(state: dict[str, torch.Tensor], prefix: str) -> list[np.ndarray]:
    return [state[f"{prefix}.{suffix}"].detach().cpu().numpy()
            for suffix in ("weight", "bias", "running_mean", "running_var")]


def dense_weights(state: dict[str, torch.Tensor], prefix: str) -> list[np.ndarray]:
    return [state[f"{prefix}.weight"].detach().cpu().numpy().T,
            state[f"{prefix}.bias"].detach().cpu().numpy()]


def build_keras(state: dict[str, torch.Tensor]) -> tf.keras.Model:
    inputs = tf.keras.Input((99, 64, 2), name="logmel_harmonic")
    value = tf.keras.layers.ZeroPadding2D(((2, 2), (1, 1)), name="stem_padding")(inputs)
    value = tf.keras.layers.Conv2D(16, (5, 3), strides=2, padding="valid", use_bias=False,
                                   name="stem_conv")(value)
    value = tf.keras.layers.BatchNormalization(epsilon=1e-5, name="stem_bn")(value)
    value = tf.keras.layers.ReLU(name="stem_relu")(value)
    specifications = ((16, 28, 2), (28, 40, 2), (40, 56, 1))
    for number, (source, target, stride) in enumerate(specifications, 1):
        value = tf.keras.layers.ZeroPadding2D(1, name=f"block{number}_padding")(value)
        value = tf.keras.layers.DepthwiseConv2D(3, strides=stride, padding="valid", use_bias=False,
                                                name=f"block{number}_depthwise")(value)
        value = tf.keras.layers.BatchNormalization(epsilon=1e-5,
                                                    name=f"block{number}_depthwise_bn")(value)
        value = tf.keras.layers.Activation(tf.nn.silu, name=f"block{number}_depthwise_silu")(value)
        value = tf.keras.layers.Conv2D(target, 1, use_bias=False, name=f"block{number}_pointwise")(value)
        value = tf.keras.layers.BatchNormalization(epsilon=1e-5,
                                                    name=f"block{number}_pointwise_bn")(value)
        value = tf.keras.layers.Activation(tf.nn.silu, name=f"block{number}_pointwise_silu")(value)
    value = tf.keras.layers.GlobalAveragePooling2D(name="average")(value)
    value = tf.keras.layers.Dense(48, name="head_dense")(value)
    value = tf.keras.layers.ReLU(name="head_relu")(value)
    value = tf.keras.layers.Dense(1, name="snore_logit")(value)
    outputs = tf.keras.layers.Activation("sigmoid", name="snore_probability")(value)
    model = tf.keras.Model(inputs, outputs, name="sleep_ai_v3")

    model.get_layer("stem_conv").set_weights(conv_weights(state, "features.0.weight"))
    model.get_layer("stem_bn").set_weights(batch_norm_weights(state, "features.1"))
    for number, feature_index in enumerate((3, 4, 5), 1):
        prefix = f"features.{feature_index}.block"
        model.get_layer(f"block{number}_depthwise").set_weights(
            depthwise_weights(state, f"{prefix}.0.weight"))
        model.get_layer(f"block{number}_depthwise_bn").set_weights(
            batch_norm_weights(state, f"{prefix}.1"))
        model.get_layer(f"block{number}_pointwise").set_weights(
            conv_weights(state, f"{prefix}.3.weight"))
        model.get_layer(f"block{number}_pointwise_bn").set_weights(
            batch_norm_weights(state, f"{prefix}.4"))
    model.get_layer("head_dense").set_weights(dense_weights(state, "head.0"))
    model.get_layer("snore_logit").set_weights(dense_weights(state, "head.2"))
    return model


def load_inputs(features_root: Path, count: int) -> tuple[np.ndarray, np.ndarray]:
    features = np.load(features_root / "features.npy", mmap_mode="r")
    harmonic = np.load(features_root / "masks.npy", mmap_mode="r")
    splits = np.load(features_root / "splits.npy", mmap_mode="r")
    indices = np.flatnonzero((splits == "validation") | (splits == "board_test"))[:count]
    values = np.stack((features[indices], harmonic[indices]), axis=-1).astype(np.float32)
    return indices, values


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--features", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--representative", type=int, default=512)
    args = parser.parse_args()

    state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    pytorch = BinaryStudentMedium().eval()
    pytorch.load_state_dict(state)
    keras_model = build_keras(state)
    indices, values = load_inputs(args.features, max(args.representative, 1024))

    with torch.no_grad():
        torch_input = torch.from_numpy(values[:256].transpose(0, 3, 1, 2))
        reference = pytorch(torch_input).sigmoid().numpy()
    keras_probability = keras_model(values[:256], training=False).numpy().reshape(-1)
    float_error = np.abs(reference - keras_probability)
    if float_error.max() > 2e-5:
        raise RuntimeError(f"PyTorch/Keras mismatch: max error {float_error.max():.8f}")

    representative = values[:args.representative]

    def representative_dataset():
        for sample in representative:
            yield [sample[None, ...]]

    converter = tf.lite.TFLiteConverter.from_keras_model(keras_model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    converted = converter.convert()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(converted)

    interpreter = tf.lite.Interpreter(model_content=converted)
    interpreter.allocate_tensors()
    input_info = interpreter.get_input_details()[0]
    output_info = interpreter.get_output_details()[0]
    input_scale, input_zero = input_info["quantization"]
    output_scale, output_zero = output_info["quantization"]
    quantized_probability = []
    for sample in values[:256]:
        quantized = np.clip(np.rint(sample / input_scale) + input_zero, -128, 127).astype(np.int8)
        interpreter.set_tensor(input_info["index"], quantized[None, ...])
        interpreter.invoke()
        raw = int(interpreter.get_tensor(output_info["index"]).reshape(-1)[0])
        quantized_probability.append((raw - output_zero) * output_scale)
    quantized_probability = np.asarray(quantized_probability)
    int8_error = np.abs(reference - quantized_probability)
    report = {
        "checkpoint": str(args.checkpoint),
        "features": str(args.features),
        "bytes": len(converted),
        "input_shape": input_info["shape"].tolist(),
        "input_quantization": [input_scale, input_zero],
        "output_quantization": [output_scale, output_zero],
        "float_max_error": float(float_error.max()),
        "int8_mean_error": float(int8_error.mean()),
        "int8_max_error": float(int8_error.max()),
        "samples": len(reference),
        "indices_sha256_input": int(indices[:256].sum()),
    }
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
