#!/usr/bin/env python3
"""Evaluate the exact embedded v1 DSP/model path on stereo board WAV files."""

from __future__ import annotations

import argparse
import math
import wave
from pathlib import Path

import numpy as np
import tensorflow as tf


SAMPLE_RATE = 48_000
SAMPLE_COUNT = 48_000
FFT_SIZE = 256
MFE_FRAMES = 99
MFE_BANDS = 40


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as source:
        if source.getframerate() != SAMPLE_RATE or source.getsampwidth() != 2:
            raise ValueError(f"{path}: expected 48 kHz, 16-bit PCM")
        channels = source.getnchannels()
        pcm = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2")
    return pcm.reshape(-1, channels).astype(np.float32) / 32768.0


def mel_bins() -> np.ndarray:
    low = 1127.0 * math.log(1.0 + 300.0 / 700.0)
    high = 1127.0 * math.log(1.0 + 24_000.0 / 700.0)
    result = []
    for index in range(MFE_BANDS + 2):
        mel = low + (high - low) * index / (MFE_BANDS + 1.0)
        hz = 700.0 * (math.exp(mel / 1127.0) - 1.0)
        if index == MFE_BANDS + 1:
            hz -= 0.001
        result.append(min(FFT_SIZE // 2, max(0, math.floor(130.0 * hz / SAMPLE_RATE))))
    return np.asarray(result, dtype=np.int32)


MEL_BINS = mel_bins()


def mix_window(stereo: np.ndarray, strategy: str) -> np.ndarray:
    if stereo.shape[1] == 1:
        return stereo[:, 0]
    left, right = stereo[:, 0], stereo[:, 1]
    if strategy == "left":
        return left
    if strategy == "right":
        return right
    if strategy == "stronger":
        return left if np.mean(left * left) >= np.mean(right * right) else right
    if strategy == "mean":
        return (left + right) * 0.5
    if strategy == "aligned_mean":
        # The two capsules are close together, but a few samples of acoustic or
        # I2S skew can still reduce a plain average. Pick one delay for the full
        # inference window, then average to suppress uncorrelated codec noise.
        best_lag, best_score = 0, -np.inf
        for lag in range(-16, 17):
            if lag < 0:
                a, b = left[-lag:], right[:lag]
            elif lag > 0:
                a, b = left[:-lag], right[lag:]
            else:
                a, b = left, right
            score = float(np.dot(a, b))
            if score > best_score:
                best_lag, best_score = lag, score
        if best_lag < 0:
            result = left.copy()
            result[-best_lag:] = (left[-best_lag:] + right[:best_lag]) * 0.5
            return result
        if best_lag > 0:
            result = right.copy()
            result[best_lag:] = (left[:-best_lag] + right[best_lag:]) * 0.5
            return result
        return (left + right) * 0.5
    raise ValueError(strategy)


def extract_mfe(pcm: np.ndarray, target_dbfs: float, maximum_gain: float) -> tuple[np.ndarray, float, float]:
    rms = float(np.sqrt(np.mean(pcm * pcm)))
    peak = float(np.max(np.abs(pcm)))
    input_dbfs = 20.0 * math.log10(rms) if rms > 1e-7 else -140.0
    gain = 1.0
    if -72.0 < input_dbfs < target_dbfs:
        gain = min(maximum_gain, 10.0 ** ((target_dbfs - input_dbfs) / 20.0))
        if peak > 0.0:
            gain = min(gain, 0.98 / peak)

    features = np.empty((MFE_FRAMES, MFE_BANDS), dtype=np.float32)
    for frame in range(MFE_FRAMES):
        offset = frame * 480
        samples = np.clip(pcm[offset : offset + FFT_SIZE] * gain, -1.0, 1.0)
        spectrum = np.fft.rfft(samples, n=FFT_SIZE)
        power = (spectrum.real * spectrum.real + spectrum.imag * spectrum.imag) / FFT_SIZE
        for band in range(MFE_BANDS):
            left, middle, right = MEL_BINS[band : band + 3]
            energy = power[middle]
            for bin_index in range(left + 1, right):
                if bin_index < middle and middle != left:
                    energy += (bin_index - left) * power[bin_index] / (middle - left)
                elif bin_index > middle and right != middle:
                    energy += (right - bin_index) * power[bin_index] / (right - middle)
            value = (10.0 * math.log10(max(float(energy), 1e-30)) + 52.0) / 64.0
            features[frame, band] = np.clip(round(value * 256.0) / 256.0, 0.0, 1.0)
    return features, input_dbfs, gain


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("wav", type=Path, nargs="+")
    args = parser.parse_args()

    interpreter = tf.lite.Interpreter(model_path=str(args.model))
    interpreter.allocate_tensors()
    input_info = interpreter.get_input_details()[0]
    output_info = interpreter.get_output_details()[0]
    in_scale, in_zero = input_info["quantization"]
    out_scale, out_zero = output_info["quantization"]

    for wav_path in args.wav:
        stereo = read_wav(wav_path)
        print(f"\n{wav_path.name}: {len(stereo) / SAMPLE_RATE:.1f} s")
        for strategy in ("left", "right", "stronger", "mean", "aligned_mean"):
            for maximum_gain in (64.0, 128.0, 256.0):
                probabilities, levels, gains = [], [], []
                for offset in range(0, len(stereo) - SAMPLE_COUNT + 1, SAMPLE_COUNT // 2):
                    mixed = mix_window(stereo[offset : offset + SAMPLE_COUNT], strategy)
                    if len(mixed) < SAMPLE_COUNT:
                        continue
                    features, level, gain = extract_mfe(mixed, -18.0, maximum_gain)
                    quantized = np.clip(np.rint(features / in_scale) + in_zero, -128, 127).astype(np.int8)
                    interpreter.set_tensor(input_info["index"], quantized.reshape(input_info["shape"]))
                    interpreter.invoke()
                    output = interpreter.get_tensor(output_info["index"]).reshape(-1)
                    probabilities.append(float((output[1] - out_zero) * out_scale))
                    levels.append(level)
                    gains.append(gain)
                if probabilities:
                    print(
                        f"  {strategy:12s} max_gain={maximum_gain:5.0f}x: "
                        f"p50={np.median(probabilities)*100:5.1f}% "
                        f"p90={np.percentile(probabilities,90)*100:5.1f}% "
                        f"max={max(probabilities)*100:5.1f}% "
                        f"level={np.mean(levels):5.1f} dBFS gain={np.mean(gains):4.1f}x"
                    )


if __name__ == "__main__":
    main()
