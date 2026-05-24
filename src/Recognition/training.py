import os
from collections import Counter

import numpy as np
import tensorflow as tf
from tensorflow.keras import layers, models
from tensorflow.keras.callbacks import EarlyStopping, ModelCheckpoint
from tensorflow.keras.models import load_model

import config

DATA_ROOT = config.IMAGE_TRAINING_FOLDER

img_height, img_width = 48, 37
batch_size = 32
EPOCHS = 100
SEED = 42

train_ds_raw = tf.keras.utils.image_dataset_from_directory(
    os.path.join(DATA_ROOT, "train"),
    color_mode="grayscale",
    image_size=(img_height, img_width),
    batch_size=batch_size,
    shuffle=True,
    seed=SEED,
)
val_ds_raw = tf.keras.utils.image_dataset_from_directory(
    os.path.join(DATA_ROOT, "val"),
    color_mode="grayscale",
    image_size=(img_height, img_width),
    batch_size=batch_size,
    shuffle=False,
)
test_ds_raw = tf.keras.utils.image_dataset_from_directory(
    os.path.join(DATA_ROOT, "test"),
    color_mode="grayscale",
    image_size=(img_height, img_width),
    batch_size=batch_size,
    shuffle=False,
)

print(f"Class names: {train_ds_raw.class_names}")

normalize = layers.Rescaling(1.0 / 255)
augment = tf.keras.Sequential([
    layers.RandomTranslation(
        height_factor=0.15, width_factor=0.0, fill_mode="nearest", seed=SEED
    ),
])


def to_onehot(x, y):
    return tf.one_hot(y, depth=10)


AUTOTUNE = tf.data.AUTOTUNE
train_ds = (
    train_ds_raw
    .map(lambda x, y: (augment(normalize(x), training=True), to_onehot(x, y)))
    .prefetch(AUTOTUNE)
)
val_ds = (
    val_ds_raw
    .map(lambda x, y: (normalize(x), to_onehot(x, y)))
    .cache()
    .prefetch(AUTOTUNE)
)
test_ds = (
    test_ds_raw
    .map(lambda x, y: (normalize(x), to_onehot(x, y)))
    .cache()
    .prefetch(AUTOTUNE)
)

model = models.Sequential([
    layers.Input(shape=(img_height, img_width, 1)),

    layers.Conv2D(32, (3, 3), padding="same", activation="relu"),
    layers.BatchNormalization(),
    layers.MaxPooling2D(2, 2),
    layers.Dropout(0.25),

    layers.Conv2D(64, (3, 3), padding="same", activation="relu"),
    layers.BatchNormalization(),
    layers.MaxPooling2D(2, 2),
    layers.Dropout(0.25),

    layers.Conv2D(128, (3, 3), padding="same", activation="relu"),
    layers.BatchNormalization(),
    layers.GlobalAveragePooling2D(),
    layers.Dropout(0.3),

    layers.Dense(10, activation="softmax"),
])

model.compile(
    optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
    loss=tf.keras.losses.CategoricalCrossentropy(label_smoothing=0.05),
    metrics=["accuracy"],
)
model.summary()

best_path = config.MODEL_BEST_PATH
callbacks = [
    EarlyStopping(monitor="val_loss", patience=8, restore_best_weights=False),
    ModelCheckpoint(best_path, monitor="val_accuracy",
                    save_best_only=True, mode="max", verbose=1),
]

history = model.fit(
    train_ds,
    validation_data=val_ds,
    epochs=EPOCHS,
    callbacks=callbacks,
)


def run_eval(m, ds, label):
    print(f"\n=== Evaluace: {label} ===")
    loss, acc = m.evaluate(ds, verbose=0)
    print(f"loss: {loss:.4f}, acc: {acc:.4f}")
    y_true_all, y_pred_all = [], []
    for x, y in ds:
        p = m.predict(x, verbose=0)
        y_true_all.extend(np.argmax(y.numpy(), axis=1).tolist())
        y_pred_all.extend(np.argmax(p, axis=1).tolist())
    y_true = np.array(y_true_all)
    y_pred = np.array(y_pred_all)
    overall = (y_true == y_pred).mean()
    print(f"manual overall acc: {overall:.4f}")
    print("Per-class accuracy:")
    for cls in range(10):
        mask = y_true == cls
        if mask.sum() > 0:
            a = (y_pred[mask] == cls).mean()
            print(f"  class {cls}: {a:.4f} ({int(mask.sum())} samples)")
    print("Confusion (jen nenulove):")
    for cls in range(10):
        mask = y_true == cls
        if mask.sum() == 0:
            continue
        row = Counter(y_pred[mask].tolist())
        print(f"  {cls}: {dict(sorted(row.items()))}")
    return overall


# 1. eval IN-MEMORY modelu na testu
acc_in_memory = run_eval(model, test_ds, "in-memory po treninku")

# 2. eval IN-MEMORY na NEcache snimcich primo z disku (jako prediction.py)
print("\n=== Eval rovnou z disku (jako prediction.py) ===")
from PIL import Image
from tensorflow.keras.utils import img_to_array
correct, total = 0, 0
for cls in range(10):
    folder = os.path.join(DATA_ROOT, "test", str(cls))
    for fn in sorted(os.listdir(folder))[:50]:
        img = Image.open(os.path.join(folder, fn)).convert("L")
        arr = img_to_array(img) / 255.0
        x = np.expand_dims(arr, 0)
        p = model.predict(x, verbose=0)
        if int(np.argmax(p)) == cls:
            correct += 1
        total += 1
acc_disk = correct / total
print(f"acc primo z disku: {acc_disk:.4f} ({correct}/{total})")

# 3. uloz, znovu nactij, eval znovu
final_path = config.MODEL_PATH
model.save(final_path)
print(f"\nUlozeno: {final_path}")

loaded = load_model(final_path)
acc_loaded = run_eval(loaded, test_ds, "po save+load .keras")

print("\n=== SOUHRN ===")
print(f"in-memory acc:    {acc_in_memory:.4f}")
print(f"primo z disku:    {acc_disk:.4f}")
print(f"po save+load:     {acc_loaded:.4f}")
if abs(acc_in_memory - acc_loaded) < 0.01:
    print("OK: save/load zachovava model.")
else:
    print("PROBLEM: save/load nezachovava model!")
