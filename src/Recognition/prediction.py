import numpy as np
from tensorflow.keras.utils import img_to_array
from tensorflow.keras.models import load_model
from PIL import Image
import json
import cv2
import pika
import os
import math
import time

import config
import config_secret

model = load_model(config.MODEL_PATH)
imageFolder = config.IMAGE_FOLDER

coords = config.DIGIT_COORDS
areas2 = config.GAUGE_AREAS

lower = np.array(config.HSV_LOWER)
upper = np.array(config.HSV_UPPER)

class_names = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]

def detect_jpeg_glitch(path, threshold=30):
    try:
        img = Image.open(path)
        img.verify()
    except Exception:
        print("Img verify")
        return False
    img2 = cv2.imread(path)
    if img2 is None:
        print("Cv2.imgread")
        return False  # nelze načíst → poškozený

    diff = np.abs(np.diff(img2.astype(np.int16), axis=0))
    diff_sum = diff.mean(axis=(1,2))

    # hledáme náhlý skok mezi řádky
    if np.max(diff_sum) > threshold:
        print("Threshold")
        return False
    return True

def preprocess_image(image_path):
    values = {}
    results = []
    values["file"] = image_path
    imageOk = detect_jpeg_glitch(os.path.join(imageFolder, image_path))
    if not imageOk:
        print(f"Image {image_path} je poškozený.")
        values["result"] = "Fail"
        return values

    img = Image.open(os.path.join(imageFolder, image_path))
    confidences = []
    for (x, y, w, h) in coords:
        # Výřez
        cropped = img.crop((x, y, x + w, y + h)).convert("L")
        cropped_np = np.array(cropped)
        cropped_np = cv2.morphologyEx(cropped_np, cv2.MORPH_CLOSE, np.ones((2,2), np.uint8))
        cropped_pil = Image.fromarray(cropped_np)
        x_input = img_to_array(cropped_pil) / 255.0
        x_input = np.expand_dims(x_input, axis=0)
        pred = model.predict(x_input)

        pred = pred[0] 
        sorted_indices = np.argsort(pred)[::-1]

        print("Predikce (od nejpravděpodobnější):")
        for i in sorted_indices:
            print(f"{class_names[i]}: {pred[i]:.4f}")
        results.append(class_names[sorted_indices[0]])
        print("Predikce:", class_names[sorted_indices[0]])
        confidences.append(pred[sorted_indices[0]])

    pocitadlo = "".join(results)
    values["value"] = pocitadlo
    # Geometrický průměr (přes exp(mean(log)) kvůli numerické stabilitě) –
    # vyjadřuje důvěru v celý odečet, penalizuje i jednu nejistou číslici.
    values["confidence"] = round(float(np.exp(np.mean(np.log(confidences)))), 4)
    print("Výsledek:", pocitadlo)
    counter = 0
    error = False
    for(x, y , w, h) in areas2:
        img2 = img.crop((x, y, x + w, y + h))
        img2_np = np.array(img2)
        img2_cv = cv2.cvtColor(img2_np, cv2.COLOR_RGB2BGR)
        hsv = cv2.cvtColor(img2_cv, cv2.COLOR_BGR2HSV)
        mask = cv2.inRange(hsv, lower, upper)
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if contours:
            cnt = max(contours, key=cv2.contourArea)
            height, width = img2_cv.shape[:2]
            center = (width // 2, height // 2)
            max_dist = 0
            tip = center
            for point in cnt:
                x, y = point[0]
                dist = np.sqrt((x - center[0])**2 + (y - center[1])**2)
                if dist > max_dist:
                    max_dist = dist
                    tip = (x, y)

            # Výpočet úhlu
            dx = tip[0] - center[0]
            dy = tip[1] - center[1]
            angle_rad = np.arctan2(dy, dx)
            angle_deg = np.degrees(angle_rad)
            angle = ((90 + angle_deg) % 360) / 36
            print(f"Úhel ručičky je: {angle:.2f}")
            values[f"value_{counter}"] = angle
        else:
            error = True
            values[f"value_{counter}"] = -1
        counter += 1
    if error:
        values["result"] = "Fail"
    else:
        values["result"] = "OK"
    return values

def get_value(value: float, previous_value: float) -> int:
    f = math.floor(value)
    d = value - f
    print(f"f: {f}, d: {d}, previous: {previous_value}")
    if d < 0.35 and previous_value >= 7:
        if int(f) - 1 < 0:
            return 9
        return int(f) - 1
    if d> 0.86 and previous_value < 4:
        if int(f) + 1 > 9:
            return 0
        return int(f) + 1

    return int(f)

# Připojení k RabbitMQ serveru
connection = pika.BlockingConnection(
    pika.ConnectionParameters(
        host=config_secret.RABBITMQ_HOST,
        port=config_secret.RABBITMQ_PORT,
        virtual_host=config_secret.RABBITMQ_VHOST,
        credentials=pika.PlainCredentials(
            config_secret.RABBITMQ_USER,
            config_secret.RABBITMQ_PASSWORD,
        ),
        heartbeat=config_secret.RABBITMQ_HEARTBEAT,
    )
)
channel = connection.channel()
queue_name = config.QUEUE_NAME
channel.queue_declare(queue=queue_name, durable=True)

def wait_for_file(filename, retries=10, interval=5):
    for attempt in range(retries):
        if os.path.exists(os.path.join(imageFolder, filename)):
            print(f"Soubor '{filename}' nalezen.")
            return True
        print(f"[{attempt + 1}/{retries}] Soubor '{filename}' zatím neexistuje, čekám {interval} s...")
        time.sleep(interval)
    print(f"Soubor '{filename}' se ani po {retries} pokusech neobjevil.")
    return False

def callback(ch, method, properties, body):
    print("Přijato:", body.decode())
    body_str = body.decode('utf-8')
    data = json.loads(body_str)
    corrId = data.get("CorrelationId")
    fileExists = wait_for_file(data["FileName"])
    if fileExists:
        result = preprocess_image(data["FileName"])
        result["gaugeId"] = data["GaugeId"]
        result["correlationId"] = corrId
        message = json.dumps(result)
        if result["result"] == "OK":
            d4 = int(math.floor(result["value_3"]))
            d3 = get_value(result["value_2"], d4)
            d2 = get_value(result["value_1"], d3)
            d1 = get_value(result["value_0"], d2)
            val = result["value"]
            value = f"{val}.{d1}{d2}{d3}{d4}"
            print(f"Final value: {value}")
            result["state"] = value
            result["datetime"] = data["Datetime"]
            message = json.dumps(result)
            channel.basic_publish(exchange=config.EXCHANGE, routing_key=config.ROUTING_KEY_SUCCESS, body=message)
        else:
            channel.basic_publish(exchange=config.EXCHANGE, routing_key=config.ROUTING_KEY_FAIL, body=message)
    else:
        notFound = {
            "file": data["FileName"],
            "gaugeId": data["GaugeId"],
            "correlationId": corrId,
            "result": "Fail",
            "value": "",
            "value_0": -1,
            "value_1": -1,
            "value_2": -1,
            "value_3": -1
        }
        message = json.dumps(notFound)
        channel.basic_publish(exchange=config.EXCHANGE, routing_key=config.ROUTING_KEY_FAIL, body=message)
    ch.basic_ack(delivery_tag=method.delivery_tag)

channel.basic_consume(
    queue=queue_name,
    on_message_callback=callback,
    auto_ack=False
)
channel.start_consuming()
