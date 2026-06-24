"""
Ověření natrénovaného modelu na existujících snímcích (dávkově) + triáž chyb.

Vezme snímky `vodomer_<hash>_<counter>,<dials>.jpg` (název = heuristikou opravený
stav), prožene výřezy číslic modelem, spočítá shodu a chyby roztřídí do kýblů,
ať se dá rychle posoudit, co je chyba modelu vs. špatný label/otrava datasetu.
Preprocessing je IDENTICKÝ s prediction.py.

Použití:
    python eval_model.py [složka] [model.keras] [--sample=N] [--limit=N] [--save-bad]

--sample=N  náhodný vzorek N snímků (na rychlé ověření ~5–10k).
--save-bad  uloží výřezy chyb do <složka>/_eval_bad/<kýbl>/ (cap na kýbl).
Vždy zapíše <složka>/eval_errors.csv se všemi neshodami.
"""
import os
import re
import sys
import csv
import random
from collections import Counter, defaultdict

import numpy as np
import cv2
from PIL import Image
from tensorflow.keras.models import load_model
from tensorflow.keras.utils import img_to_array

import config

FILE_RE = re.compile(r"^vodomer_([0-9a-fA-F]+)_(\d{1,5}),(\d{4})\.jpg$")
COORDS = config.DIGIT_COORDS
N = len(COORDS)
CHUNK_IMAGES = 256
SEED = 42
MAX_SAVE_PER_BUCKET = 300

# Prahy pro triáž
LABEL_SUSPECT_PPRED = 0.85   # model si je hodně jistý...
LABEL_SUSPECT_PGT = 0.05     # ...a správné číslici dal skoro nulu
LOW_CONF_MAX = 0.50


def crop_digit(img, coord):
    x, y, w, h = coord
    cropped = img.crop((x, y, x + w, y + h)).convert("L")
    arr = np.array(cropped)
    arr = cv2.morphologyEx(arr, cv2.MORPH_CLOSE, np.ones((2, 2), np.uint8))
    return img_to_array(Image.fromarray(arr)) / 255.0


def adjacent(a, b):
    return abs(a - b) == 1 or {a, b} == {0, 9}


def bucket_of(pos, gt, pred, p_gt, p_pred, max_p):
    # pořadí záleží: roll-over na pos4 řešíme dřív než label_suspect,
    # protože tam je label hodnotově správný (jen viditelná číslice ≠ pravá).
    if pos == N - 1 and adjacent(gt, pred):
        return "rollover_pos4"
    if p_pred >= LABEL_SUSPECT_PPRED and p_gt <= LABEL_SUSPECT_PGT:
        return "label_suspect"
    if max_p < LOW_CONF_MAX:
        return "low_conf"
    return "model_error"


def gather(folder):
    out = []
    for root, _dirs, files in os.walk(folder):
        if os.path.basename(root) == "_eval_bad":
            continue
        for fn in files:
            m = FILE_RE.match(fn)
            if m:
                out.append((os.path.join(root, fn), m.group(2).zfill(N)))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a.split("=")[0]: (a.split("=")[1] if "=" in a else True)
             for a in sys.argv[1:] if a.startswith("--")}
    folder = args[0] if len(args) > 0 else config.IMAGE_FOLDER_SUCCESS
    model_path = args[1] if len(args) > 1 else config.MODEL_PATH
    sample = int(flags["--sample"]) if "--sample" in flags else None
    limit = int(flags["--limit"]) if "--limit" in flags else None
    save_bad = "--save-bad" in flags
    bad_dir = os.path.join(folder, "_eval_bad")

    print(f"Model:  {model_path}")
    print(f"Složka: {folder}")
    model = load_model(model_path)

    items = gather(folder)
    print(f"Nalezeno snímků: {len(items)}")
    if sample and sample < len(items):
        random.Random(SEED).shuffle(items)
        items = items[:sample]
        print(f"Vzorek: {len(items)}")
    elif limit:
        items = items[:limit]
    if not items:
        return

    total = correct = 0
    images_done = exact_match = 0
    pos_total = [0] * N
    pos_correct = [0] * N
    cls_total = Counter()
    cls_correct = Counter()
    confusion = defaultdict(Counter)
    gt_prob_ok, gt_prob_bad = [], []
    bucket_counts = Counter()
    bucket_saved = Counter()
    errors_csv = []   # (file, pos, gt, pred, p_gt, p_pred, bucket)

    for start in range(0, len(items), CHUNK_IMAGES):
        chunk = items[start:start + CHUNK_IMAGES]
        crops, meta, opened = [], [], set()
        for li, (path, gt) in enumerate(chunk):
            try:
                img = Image.open(path)
                cs = [crop_digit(img, c) for c in COORDS]
            except Exception as e:
                print(f"  WARN {os.path.basename(path)}: {e}")
                continue
            opened.add(li)
            for pos in range(N):
                crops.append(cs[pos])
                meta.append((li, pos, int(gt[pos]), os.path.basename(path)))
        if not crops:
            continue

        preds = model.predict(np.stack(crops), verbose=0)
        pred_d = preds.argmax(axis=1)
        bad_li = set()
        for k, (li, pos, g, fn) in enumerate(meta):
            p = int(pred_d[k])
            pg = float(preds[k][g])
            pp = float(preds[k][p])
            total += 1
            pos_total[pos] += 1
            cls_total[g] += 1
            confusion[g][p] += 1
            if p == g:
                correct += 1
                pos_correct[pos] += 1
                cls_correct[g] += 1
                gt_prob_ok.append(pg)
            else:
                bad_li.add(li)
                gt_prob_bad.append(pg)
                b = bucket_of(pos, g, p, pg, pp, float(preds[k].max()))
                bucket_counts[b] += 1
                errors_csv.append((fn, pos, g, p, round(pg, 3), round(pp, 3), b))
                if save_bad and bucket_saved[b] < MAX_SAVE_PER_BUCKET:
                    d = os.path.join(bad_dir, b)
                    os.makedirs(d, exist_ok=True)
                    crop = (crops[k][:, :, 0] * 255).astype(np.uint8)
                    big = cv2.resize(crop, (COORDS[pos][2] * 6, COORDS[pos][3] * 6),
                                     interpolation=cv2.INTER_NEAREST)
                    cv2.imwrite(os.path.join(d, f"gt{g}_pred{p}_pos{pos}_{fn}.png"), big)
                    bucket_saved[b] += 1
        images_done += len(opened)
        exact_match += len(opened) - len(bad_li)
        print(f"  ... {min(start + CHUNK_IMAGES, len(items))}/{len(items)} "
              f"(přesnost číslic {correct/total:.4f})")

    # CSV se všemi neshodami
    csv_path = os.path.join(folder, "eval_errors.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["file", "pos", "gt", "pred", "p_gt", "p_pred", "bucket"])
        w.writerows(errors_csv)

    # ---- report ----
    print("\n" + "=" * 60)
    print(f"Přesnost číslic:   {correct}/{total} = {correct/total:.4f}")
    print(f"Celý odečet (5/5): {exact_match}/{images_done} = {exact_match/images_done:.4f}")
    print("\nPřesnost podle POZICE (digit4 = rolující jednotka):")
    for i in range(N):
        a = pos_correct[i] / pos_total[i] if pos_total[i] else 0
        print(f"  pozice {i}: {a:.4f} ({pos_correct[i]}/{pos_total[i]})")
    print("\nConfusion (gt -> predikce, jen nenulové):")
    for g in range(10):
        if cls_total[g]:
            print(f"  {g}: {dict(sorted(confusion[g].items()))}")

    total_err = sum(bucket_counts.values())
    print(f"\nTRIÁŽ CHYB ({total_err} chybných číslic):")
    print(f"  label_suspect : {bucket_counts['label_suspect']:>6}  "
          f"(model si jistý jinou číslicí → nejspíš špatný label / otrava)")
    print(f"  rollover_pos4 : {bucket_counts['rollover_pos4']:>6}  "
          f"(rolující jednotka, label hodnotově OK, řeší heuristika)")
    print(f"  low_conf      : {bucket_counts['low_conf']:>6}  "
          f"(model nejistý, max p < {LOW_CONF_MAX})")
    print(f"  model_error   : {bucket_counts['model_error']:>6}  "
          f"(praví kandidáti na chybu modelu → přezkoumat)")
    print(f"\nVšechny neshody: {csv_path}")
    if save_bad:
        print(f"Výřezy po kýblech: {bad_dir}/<kýbl>/ (max {MAX_SAVE_PER_BUCKET} na kýbl)")


if __name__ == "__main__":
    main()
