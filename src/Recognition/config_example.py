# Zkopíruj tento soubor jako config.py a uprav hodnoty podle své instalace.
# config.py je v .gitignore - obsahuje hodnoty specifické pro tvoji kameru
# a topologii RabbitMQ.

# Cesta ke složce, kam ESP32-CAM ukládá snímky.
IMAGE_FOLDER = r"C:\path\to\gauge\images"
IMAGE_FOLDER_SUCCESS = r"C:\path\to\gauge\images\success"
IMAGE_TRAINING_FOLDER = r"C:\path\to\gauge\images\training"


# Cesta k Keras modelu pro rozpoznání číslic.
MODEL_PATH = r"C:\path\to\prediction\watersensor.keras"
MODEL_BEST_PATH = r"C:\path\to\prediction\watersensor_best.keras"

# Výřezy číslic na měřáku (x, y, w, h).
# Nakalibruj podle pozice kamery vůči číselníku - každý tuple je jedno políčko
# mechanického počítadla.
DIGIT_COORDS = [
    (253, 159, 37, 48),
    (299, 159, 37, 48),
    (346, 159, 37, 48),
    (397, 159, 37, 48),
    (445, 159, 37, 48),
]

# Výřezy ručičkových ukazatelů (x, y, w, h).
# Pořadí odpovídá řádům 0.x, 0.0x, 0.00x, 0.000x (od nejvyššího po nejnižší řád).
GAUGE_AREAS = [
    (491, 228, 106, 103),
    (454, 346, 106, 103),
    (337, 395, 106, 103),
    (219, 344, 106, 103),
]

# HSV rozsah pro detekci barvy ručičky (uprav podle barvy ručiček na měřáku).
# Výchozí hodnoty cílí na oranžovo-červenou.
HSV_LOWER = [10, 50, 50]
HSV_UPPER = [40, 255, 255]

# RabbitMQ topologie - musí odpovídat tomu, co máš nastavené na brokeru.
EXCHANGE = "WaterSensor"
QUEUE_NAME = "recognize"
ROUTING_KEY_SUCCESS = "recognize.success"
ROUTING_KEY_FAIL = "recognize.fail"
