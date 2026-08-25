#!/usr/bin/env python3
"""
Crop Sentinel AI — Phase 4, Steps 2+3: dataset + TinyML training

Reproduces the EXACT SAME feature algorithm as extractFeatures() in
esp32_feature_extractor_addon.ino (4x4 grid, mean R/G/B + green-ratio per
cell), runs it over a PlantVillage-style folder-per-class dataset, trains
a logistic regression classifier, evaluates it on a held-out split, and
writes a ready-to-drop-in tiny_model.h for the Arduino Uno.

****** KEEP GRID_SIZE IN SYNC WITH THE ESP32 SIDE ******
If you change GRID_SIZE (or the feature layout) here, you MUST make the
identical change in extractFeatures() in the .ino file, and retrain.

Usage:
    python train_tiny_model.py --data-dir /path/to/PlantVillage \
        --resize-w 96 --resize-h 96 --out-dir ./out
    (Get the real --resize-w/--resize-h values from the ESP32 sketch's boot
    log line: "Classifier input resolution: WxH" -- 96x96 above is just an
    example, not a default to trust blindly.)

Expected --data-dir layout (standard PlantVillage structure):
    PlantVillage/
        Pepper__bell___healthy/
            img1.jpg ...
        Pepper__bell___Bacterial_spot/
            img1.jpg ...
        Potato___healthy/
            ...
        Potato___Early_blight/
            ...
        Potato___Late_blight/
            ...

Label rule (binary, matching the Phase 4 plan): any folder name containing
"healthy" (case-insensitive) is label 0, everything else is label 1
("diseased"). This matches the 4-class Phase 2 CNN's classes collapsed to
a binary verification target for the Uno's second-opinion model.

Dependencies: numpy, pillow, scikit-learn
    pip install --break-system-packages numpy pillow scikit-learn
"""

import argparse
import csv
import math
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score, confusion_matrix, classification_report

# ---------------------------------------------------------------------
# Must match GRID_SIZE / STATS_PER_CELL / FEATURE_COUNT in the .ino file.
#
# FEATURE SET v2: [meanBrightness, greenRatio, edgeDensity, contrast] per
# cell -- two color cues, two texture cues. Replaces v1's pure
# [meanR, meanG, meanB, greenRatio]. See the matching comment block above
# extractFeatures() in esp32_phase2_3_4.ino for the full rationale.
#
# IMPORTANT: any tiny_model.h trained against v1 features is invalid
# against v2 -- the array is still 64 uint8_t values either way, so a
# stale model will compile and run, just produce meaningless predictions.
# Always retrain after a feature-set change.
# ---------------------------------------------------------------------
GRID_SIZE = 4
STATS_PER_CELL = 4  # meanBrightness, greenRatio, edgeDensity, contrast
FEATURE_COUNT = GRID_SIZE * GRID_SIZE * STATS_PER_CELL  # 64

# Resize target -- MUST match EI_CLASSIFIER_INPUT_WIDTH/HEIGHT on the ESP32,
# NOT the raw camera capture resolution. ei_camera_capture() resizes
# snapshot_buf IN PLACE down to the classifier's input size before
# extractFeatures() ever sees it, so that's the resolution this script needs
# to replicate too. The integrated Phase 2 sketch prints this value at boot
# ("Classifier input resolution: WxH") -- pass those exact numbers via
# --resize-w/--resize-h below. There is deliberately no default here: a
# wrong guess would silently train a model on the wrong scale.


def extract_features(img_rgb: np.ndarray) -> np.ndarray:
    """
    Mirrors extractFeatures() in esp32_phase2_3_4.ino exactly (feature set
    v2): 4x4 grid, per-cell [meanBrightness, greenRatio, edgeDensity,
    contrast], in the same row-major cell order.

    img_rgb: HxWx3 uint8 array, RGB order.
    Returns: (FEATURE_COUNT,) uint8 array.
    """
    height, width, _ = img_rgb.shape
    cell_w = width // GRID_SIZE
    cell_h = height // GRID_SIZE

    features = np.zeros(FEATURE_COUNT, dtype=np.uint8)
    out_idx = 0

    for gy in range(GRID_SIZE):
        for gx in range(GRID_SIZE):
            start_x = gx * cell_w
            start_y = gy * cell_h
            end_x = width if gx == GRID_SIZE - 1 else start_x + cell_w
            end_y = height if gy == GRID_SIZE - 1 else start_y + cell_h

            cell = img_rgb[start_y:end_y, start_x:end_x, :].astype(np.int64)
            r = cell[:, :, 0]
            g = cell[:, :, 1]
            b = cell[:, :, 2]
            count = cell.shape[0] * cell.shape[1]

            # Integer grayscale, matching the ESP32's (r+g+b)/3 with the
            # same truncating integer division.
            gray = (r + g + b) // 3

            sum_r = int(r.sum())
            sum_g = int(g.sum())
            sum_b = int(b.sum())
            sum_gray = int(gray.sum())
            sum_gray_sq = int((gray.astype(np.int64) ** 2).sum())

            mean_brightness = (sum_gray // count) if count > 0 else 0

            total = sum_r + sum_g + sum_b
            green_ratio = ((sum_g * 255) // total) if total > 0 else 0

            # Horizontal edge density: mean abs brightness jump between
            # horizontally adjacent pixels, same row -- matches the ESP32's
            # row-by-row scan (no wraparound between rows).
            if gray.shape[1] > 1:
                h_diff = np.abs(np.diff(gray.astype(np.int64), axis=1))
                sum_h_edge = int(h_diff.sum())
            else:
                sum_h_edge = 0
            edge_density = min(255, (sum_h_edge // count)) if count > 0 else 0

            # Contrast: population std-dev of brightness within the cell,
            # via integer sqrt of integer variance -- matches the ESP32's
            # isqrt()-based computation (not numpy's float std()).
            if count > 0:
                mean_gray_l = sum_gray // count
                variance = (sum_gray_sq // count) - (mean_gray_l * mean_gray_l)
                variance = max(0, variance)
                contrast = int(math.isqrt(variance))
            else:
                contrast = 0

            features[out_idx] = mean_brightness
            features[out_idx + 1] = green_ratio
            features[out_idx + 2] = edge_density
            features[out_idx + 3] = contrast
            out_idx += 4

    return features


def label_for_class_dir(dirname: str) -> int:
    """0 = healthy, 1 = diseased, based on folder name (case-insensitive)."""
    return 0 if "healthy" in dirname.lower() else 1


def build_dataset(data_dir: Path):
    image_paths = []
    labels = []
    class_dirs = sorted([d for d in data_dir.iterdir() if d.is_dir()])

    if not class_dirs:
        print(f"[ERROR] No class subfolders found under {data_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Found {len(class_dirs)} class folders:")
    for d in class_dirs:
        label = label_for_class_dir(d.name)
        files = [f for f in d.iterdir() if f.suffix.lower() in (".jpg", ".jpeg", ".png")]
        print(f"  {d.name:45s} -> label {label} ({'healthy' if label == 0 else 'diseased'})  [{len(files)} images]")
        for f in files:
            image_paths.append(f)
            labels.append(label)

    return image_paths, labels


def extract_dataset_features(image_paths, labels, resize_w, resize_h, log_every=200):
    X = np.zeros((len(image_paths), FEATURE_COUNT), dtype=np.uint8)
    y = np.array(labels, dtype=np.int64)

    for i, path in enumerate(image_paths):
        try:
            img = Image.open(path).convert("RGB").resize((resize_w, resize_h), Image.BILINEAR)
            X[i] = extract_features(np.array(img))
        except Exception as e:
            print(f"[WARN] Skipping unreadable image {path}: {e}", file=sys.stderr)
        if (i + 1) % log_every == 0:
            print(f"[INFO] Extracted features from {i + 1}/{len(image_paths)} images...")

    return X, y


def quantize_model(model: LogisticRegression, scale: int = 256):
    """
    Converts sklearn's float coefficients into fixed-point int16 weights
    and an int32 bias for AVR, at the given Q-scale (default Q8, scale=256).

    score_fixed = bias_fixed + sum(weight_fixed[i] * feature[i])
    real_score  = score_fixed / scale
    prediction  = "diseased" if real_score > 0 else "healthy"
      (this works because sklearn's decision boundary is exactly
       intercept + sum(coef*x) == 0 -- no need to compute sigmoid on-device
       just to get the class; sigmoid is only needed for a confidence %)
    """
    coef = model.coef_[0]  # (FEATURE_COUNT,)
    intercept = model.intercept_[0]

    weights_fixed = np.round(coef * scale).astype(np.int32)
    # Clip to int16 range -- warn if anything got clipped, since that would
    # silently degrade the model (shouldn't happen with normalized 0-255
    # features and a sane scale, but cheap to check).
    clipped = np.any((weights_fixed < -32768) | (weights_fixed > 32767))
    if clipped:
        print("[WARN] Some weights overflowed int16 at this scale -- consider lowering `scale`.", file=sys.stderr)
    weights_fixed = np.clip(weights_fixed, -32768, 32767).astype(np.int16)

    bias_fixed = int(round(intercept * scale))

    return weights_fixed, bias_fixed, scale


def write_tiny_model_header(out_path: Path, weights_fixed, bias_fixed, scale, feature_count):
    lines = []
    lines.append("// AUTO-GENERATED by train_tiny_model.py -- do not hand-edit.")
    lines.append("// Regenerate this file if you retrain the model or change GRID_SIZE.")
    lines.append("#ifndef TINY_MODEL_H")
    lines.append("#define TINY_MODEL_H")
    lines.append("")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append(f"#define FEATURE_COUNT {feature_count}")
    lines.append(f"#define MODEL_SCALE {scale}  // fixed-point scale (Qn) applied to weights and bias")
    lines.append("")
    weights_str = ", ".join(str(int(w)) for w in weights_fixed)
    lines.append(f"static const int16_t MODEL_WEIGHTS[FEATURE_COUNT] = {{ {weights_str} }};")
    lines.append(f"static const int32_t MODEL_BIAS = {int(bias_fixed)};")
    lines.append("")
    lines.append("#endif // TINY_MODEL_H")

    out_path.write_text("\n".join(lines) + "\n")
    print(f"[OK] Wrote {out_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data-dir", required=True, type=Path, help="Path to PlantVillage-style dataset root")
    parser.add_argument("--resize-w", required=True, type=int,
                         help="Must match EI_CLASSIFIER_INPUT_WIDTH printed by the ESP32 sketch at boot -- NOT the raw camera resolution")
    parser.add_argument("--resize-h", required=True, type=int,
                         help="Must match EI_CLASSIFIER_INPUT_HEIGHT printed by the ESP32 sketch at boot -- NOT the raw camera resolution")
    parser.add_argument("--out-dir", default=Path("./out"), type=Path, help="Where to write dataset.csv and tiny_model.h")
    parser.add_argument("--test-split", type=float, default=0.2, help="Held-out test fraction")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--scale", type=int, default=256, help="Fixed-point scale for the AVR model (default Q8 = 256)")
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[INFO] Feature layout: {GRID_SIZE}x{GRID_SIZE} grid, {STATS_PER_CELL} stats/cell = {FEATURE_COUNT} features")
    print(f"[INFO] Scanning {args.data_dir} ...")
    image_paths, labels = build_dataset(args.data_dir)
    print(f"[INFO] Total images: {len(image_paths)}")

    print(f"[INFO] Resizing all images to {args.resize_w}x{args.resize_h} before feature extraction "
          f"(this must match EI_CLASSIFIER_INPUT_WIDTH/HEIGHT on the ESP32, not the raw capture size)")
    print("[INFO] Extracting features (this mirrors the ESP32's extractFeatures() exactly)...")
    X, y = extract_dataset_features(image_paths, labels, args.resize_w, args.resize_h)

    # Save the raw dataset for reuse/inspection without re-scanning images.
    csv_path = args.out_dir / "dataset.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([f"f{i}" for i in range(FEATURE_COUNT)] + ["label"])
        for row, label in zip(X, y):
            writer.writerow(list(row) + [label])
    print(f"[OK] Wrote {csv_path}")

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=args.test_split, random_state=args.seed, stratify=y
    )
    print(f"[INFO] Train: {len(X_train)}  Test (held-out): {len(X_test)}")

    print("[INFO] Training logistic regression...")
    model = LogisticRegression(max_iter=2000, class_weight="balanced")
    model.fit(X_train, y_train)

    y_pred = model.predict(X_test)
    acc = accuracy_score(y_test, y_pred)
    cm = confusion_matrix(y_test, y_pred)
    report = classification_report(y_test, y_pred, target_names=["healthy", "diseased"], digits=3)

    print("\n=== Held-out evaluation (Phase 4 acceptance test) ===")
    print(f"Test accuracy: {acc * 100:.1f}%")
    print("Confusion matrix (rows=actual, cols=predicted) [healthy, diseased]:")
    print(cm)
    print(report)

    if acc < 0.75:
        print(
            "[NOTE] Accuracy is low. Per the Phase 4 plan: don't spend hours tuning "
            "the classifier itself first -- investigate the feature representation "
            "(e.g. try a coarser/finer GRID_SIZE) before assuming the model is the problem."
        )

    weights_fixed, bias_fixed, scale = quantize_model(model, scale=args.scale)
    header_path = args.out_dir / "tiny_model.h"
    write_tiny_model_header(header_path, weights_fixed, bias_fixed, scale, FEATURE_COUNT)

    print(f"\n[DONE] Copy {header_path} into uno_tinyml/ (replacing the placeholder) and reflash the Uno.")


if __name__ == "__main__":
    main()
