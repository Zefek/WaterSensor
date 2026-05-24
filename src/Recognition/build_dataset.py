import os
import re
import random
import hashlib
from collections import defaultdict

import numpy as np
import cv2
from PIL import Image
import config

TRAIN_CAP_PER_CLASS = 3000
VAL_CAP_PER_CLASS = 500
TEST_CAP_PER_CLASS = 500
TRAIN_RATIO = 0.80
VAL_RATIO = 0.10
SEED = 42

# Akceptuje obe varianty: "_00563,3100.jpg" i "_585,6966.jpg"
FILE_RE = re.compile(r"^vodomer_([0-9a-fA-F]+)_(\d{1,5}),(\d{4})\.jpg$")


def gather_inputs():
    """Vrati seznam (hash, digits_list, full_path)."""
    entries = []
    for day_folder in sorted(os.listdir(config.IMAGE_FOLDER_SUCCESS)):
        day_path = os.path.join(config.IMAGE_FOLDER_SUCCESS, day_folder)
        if not os.path.isdir(day_path):
            continue
        for fname in os.listdir(day_path):
            m = FILE_RE.match(fname)
            if not m:
                continue
            hash_, counter, _dials = m.groups()
            counter_padded = counter.zfill(5)
            digits = [int(c) for c in counter_padded]
            entries.append((hash_, digits, os.path.join(day_path, fname)))
    return entries


def split_by_hash(entries):
    """Deterministicky hash-based split 80/10/10. Cely snimek do jednoho splitu."""
    train, val, test = [], [], []
    for e in entries:
        hash_ = e[0]
        # md5 hashe filename hashe -> stabilni float v [0, 1)
        h = hashlib.md5(hash_.encode("ascii")).hexdigest()
        r = int(h[:8], 16) / 0x100000000
        if r < TRAIN_RATIO:
            train.append(e)
        elif r < TRAIN_RATIO + VAL_RATIO:
            val.append(e)
        else:
            test.append(e)
    return train, val, test


def crop_digit(img_pil: Image.Image, coord) -> Image.Image:
    """Identicky preprocessing jako v prediction.py - kvuli konzistenci train/infer."""
    x, y, w, h = coord
    cropped = img_pil.crop((x, y, x + w, y + h)).convert("L")
    cropped_np = np.array(cropped)
    cropped_np = cv2.morphologyEx(
        cropped_np, cv2.MORPH_CLOSE, np.ones((2, 2), np.uint8)
    )
    return Image.fromarray(cropped_np)


def ensure_dirs(root):
    for d in range(10):
        os.makedirs(os.path.join(root, str(d)), exist_ok=True)


def process_split(entries, split_name: str, cap_per_class):
    split_root = os.path.join(config.IMAGE_TRAINING_FOLDER, split_name)
    ensure_dirs(split_root)

    # 1) Zbal vsechny kandidaty per class.
    per_class = defaultdict(list)
    for (hash_, digits, path) in entries:
        for pos, label in enumerate(digits):
            per_class[label].append((path, hash_, pos))

    # 2) Cap per class (jen pokud limit).
    rng = random.Random(SEED)
    final = []
    for label in range(10):
        candidates = per_class[label]
        if cap_per_class is not None and len(candidates) > cap_per_class:
            chosen = rng.sample(candidates, cap_per_class)
        else:
            chosen = candidates
        for (path, hash_, pos) in chosen:
            final.append((path, hash_, pos, label))

    # 3) Group by source path - kazdy snimek otevri max jednou.
    by_path = defaultdict(list)
    for (path, hash_, pos, label) in final:
        by_path[path].append((hash_, pos, label))

    saved = 0
    failed_open = 0
    for path, items in by_path.items():
        try:
            with Image.open(path) as img:
                img.load()
                for (hash_, pos, label) in items:
                    crop_img = crop_digit(img, config.DIGIT_COORDS[pos])
                    out_path = os.path.join(
                        split_root, str(label), f"{hash_}_{pos}.jpg"
                    )
                    crop_img.save(out_path, "JPEG", quality=95)
                    saved += 1
        except Exception as e:
            failed_open += 1
            print(f"  WARN open failed: {os.path.basename(path)}: {e}")

    print(f"[{split_name}] snimku: {len(entries)}, "
          f"failed open: {failed_open}, ulozenych crops: {saved}")
    for label in range(10):
        n = len(os.listdir(os.path.join(split_root, str(label))))
        print(f"  trida {label}: {n}")


def main():
    print(f"Skenuji {config.IMAGE_FOLDER_SUCCESS} ...")
    entries = gather_inputs()
    print(f"Nalezeno {len(entries)} snimku s validnim nazvem.\n")

    train, val, test = split_by_hash(entries)
    print(f"Train: {len(train)} snimku, Val: {len(val)} snimku, "
          f"Test: {len(test)} snimku\n")

    process_split(train, "train", cap_per_class=TRAIN_CAP_PER_CLASS)
    print()
    process_split(val, "val", cap_per_class=VAL_CAP_PER_CLASS)
    print()
    process_split(test, "test", cap_per_class=TEST_CAP_PER_CLASS)


if __name__ == "__main__":
    main()
