import functools
import json
import math
import os
import secrets
import ssl
import threading
import time

import config
import config_secret
import cv2
import numpy as np
import pika
from PIL import Image
from tensorflow.keras.models import load_model
from tensorflow.keras.utils import img_to_array

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
    except (OSError, ValueError, SyntaxError):
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
    digit_probs = []   # per pozici plný softmax (10 prstů) – pro přepočet confidence po opravě heuristikou
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
        digit_probs.append([round(float(p), 4) for p in pred])

    pocitadlo = "".join(results)
    values["value"] = pocitadlo
    # Geometrický průměr (přes exp(mean(log)) kvůli numerické stabilitě) –
    # vyjadřuje důvěru v celý odečet, penalizuje i jednu nejistou číslici.
    # POZOR: tohle je confidence SUROVÉ CNN predikce (argmax číslic). Když stav
    # opraví downstream heuristika, tahle hodnota patří k PŮVODNÍ (často chybné)
    # predikci a důvěru NADHODNOCUJE. Skutečnou confidence reportované hodnoty
    # dopočítej downstream z digit_probs[pozice][opravená_číslice].
    values["cnn_confidence"] = round(float(np.exp(np.mean(np.log(confidences)))), 4)
    values["confidence"] = values["cnn_confidence"]   # zpětná kompatibilita; přepočítej po opravě
    values["digit_probs"] = digit_probs               # 5×10 softmax pro přepočet confidence
    print("Výsledek:", pocitadlo)
    error = False
    for counter, (x, y, w, h) in enumerate(areas2):
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
    if error:
        values["result"] = "Fail"
    else:
        values["result"] = "OK"
    return values

def score_candidates(value: float, previous_value):
    # Ručička má fyzicky stát na pozici (číslice + nižší_řád/10). Vlivem geometrie a
    # úhlu kamery ale může číst posunutě i o ~0.4, takže pevný práh přechod přes nulu
    # spolehlivě nepodchytí. Místo toho vybereme z kandidátů f-1, f, f+1 tu číslici,
    # jejíž očekávaná poloha (číslice + previous_value/10) nejlíp sedí na naměřený úhel.
    # previous_value=None = nejnižší řád, pod ním už žádná ručička není → expected 0.
    f = math.floor(value)
    d = value - f
    expected = 0.0 if previous_value is None else previous_value / 10.0
    candidates = [
        (abs(d - expected),     f),       # ručička sedí na své číslici
        (abs(d - expected - 1), f + 1),   # ručička zaostává → už přetočila
        (abs(d - expected + 1), f - 1),   # ručička předbíhá → ještě nepřetočila
    ]
    candidates.sort(key=lambda c: c[0])
    return f, d, expected, candidates

def get_value(value: float, previous_value: float) -> int:
    f, d, expected, candidates = score_candidates(value, previous_value)
    print(f"f: {f}, d: {d}, previous: {previous_value}, expected: {expected}")
    return candidates[0][1] % 10

def gauge_detail(pos: int, value: float, digit: int, previous_value) -> dict:
    _, d, expected, candidates = score_candidates(value, previous_value)
    return {
        "pos": pos,
        "angle": round(float(value), 4),
        "frac": round(float(d), 4),
        "expected": round(float(expected), 4),
        "digit": digit,
        "candidates": [
            {"digit": c % 10, "delta": round(float(delta), 4)}
            for delta, c in candidates
        ],
        # Odstup nejlepšího kandidáta od druhého: ~0 = těsná volba, číslice může
        # být o jedna vedle. Reportovaný "digit" u nejnižšího řádu vzniká floor(),
        # takže nemusí souhlasit s candidates[0] – i to je informace.
        "margin": round(float(candidates[1][0] - candidates[0][0]), 4),
    }

def wait_for_file(filename, retries=10, interval=5):
    for attempt in range(retries):
        if os.path.exists(os.path.join(imageFolder, filename)):
            print(f"Soubor '{filename}' nalezen.")
            return True
        print(f"[{attempt + 1}/{retries}] Soubor '{filename}' zatím neexistuje, čekám {interval} s...")
        time.sleep(interval)
    print(f"Soubor '{filename}' se ani po {retries} pokusech neobjevil.")
    return False

def build_result(body):
    data = json.loads(body.decode("utf-8"))
    corrId = data.get("CorrelationId")
    fileExists = wait_for_file(data["FileName"])
    if fileExists:
        result = preprocess_image(data["FileName"])
        result["gaugeId"] = data["GaugeId"]
        result["correlationId"] = corrId
        if result["result"] == "OK":
            d4 = math.floor(result["value_3"])
            d3 = get_value(result["value_2"], d4)
            d2 = get_value(result["value_1"], d3)
            d1 = get_value(result["value_0"], d2)
            val = result["value"]
            value = f"{val}.{d1}{d2}{d3}{d4}"
            print(f"Final value: {value}")
            result["state"] = value
            result["datetime"] = data["Datetime"]
            return config.ROUTING_KEY_SUCCESS, json.dumps(result)
        return config.ROUTING_KEY_FAIL, json.dumps(result)
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
    return config.ROUTING_KEY_FAIL, json.dumps(notFound)

def publish_and_ack(channel, delivery_tag, routing_key, message):
    try:
        if not channel.is_open:
            print("Kanál je zavřený, zprávu nelze potvrdit – doručí se znovu po obnově spojení.")
            return
        channel.basic_publish(exchange=config.EXCHANGE, routing_key=routing_key, body=message)
        channel.basic_ack(delivery_tag=delivery_tag)
    except Exception as ex:
        print(f"Publikace/ACK selhaly, zpráva se doručí znovu: {ex}")

def reject(channel, delivery_tag):
    try:
        if channel.is_open:
            channel.basic_nack(delivery_tag=delivery_tag, requeue=False)
    except Exception as ex:
        print(f"NACK selhal: {ex}")

def process_message(connection, channel, delivery_tag, body):
    try:
        routing_key, message = build_result(body)
        finish = functools.partial(publish_and_ack, channel, delivery_tag, routing_key, message)
    except Exception as ex:
        print(f"Chyba při zpracování zprávy, bude zahozena: {ex}")
        finish = functools.partial(reject, channel, delivery_tag)
    connection.add_callback_threadsafe(finish)

def on_message(connection, channel, method, properties, body):
    print("Přijato:", body.decode("utf-8", errors="replace"))
    worker = threading.Thread(
        target=process_message,
        args=(connection, channel, method.delivery_tag, body),
        daemon=True,
    )
    worker.start()

def build_connection_parameters():
    credentials = pika.PlainCredentials(
        config_secret.RABBITMQ_USER,
        config_secret.RABBITMQ_PASSWORD,
    )
    ssl_options = None
    if getattr(config_secret, "RABBITMQ_USE_TLS", False):
        ca_file = getattr(config_secret, "RABBITMQ_TLS_CA_FILE", None)
        context = ssl.create_default_context(cafile=ca_file)
        server_name = getattr(config_secret, "RABBITMQ_TLS_SERVER_NAME", None) or config_secret.RABBITMQ_HOST
        ssl_options = pika.SSLOptions(context, server_hostname=server_name)
    return pika.ConnectionParameters(
        host=config_secret.RABBITMQ_HOST,
        port=config_secret.RABBITMQ_PORT,
        virtual_host=config_secret.RABBITMQ_VHOST,
        credentials=credentials,
        heartbeat=config_secret.RABBITMQ_HEARTBEAT,
        ssl_options=ssl_options,
    )

def main():
    reconnect_delay = 1
    while True:
        connection = None
        try:
            print(f"Připojuji se k RabbitMQ na {config_secret.RABBITMQ_HOST}:{config_secret.RABBITMQ_PORT}...")
            connection = pika.BlockingConnection(build_connection_parameters())
            channel = connection.channel()
            channel.queue_declare(queue=config.QUEUE_NAME, durable=True)
            channel.basic_qos(prefetch_count=1)
            channel.basic_consume(
                queue=config.QUEUE_NAME,
                on_message_callback=functools.partial(on_message, connection),
                auto_ack=False,
            )
            print("Připojeno, čekám na zprávy.")
            reconnect_delay = 1
            channel.start_consuming()
        except KeyboardInterrupt:
            print("Ukončuji na žádost uživatele...")
            if connection is not None and connection.is_open:
                connection.close()
            break
        except (pika.exceptions.AMQPConnectionError,
                pika.exceptions.StreamLostError,
                pika.exceptions.ConnectionClosedByBroker,
                pika.exceptions.ChannelClosedByBroker) as ex:
            print(f"Spojení s RabbitMQ přerušeno: {ex}")
        except Exception as ex:
            print(f"Neočekávaná chyba spojení: {ex}")
        finally:
            try:
                if connection is not None and connection.is_open:
                    connection.close()
            except Exception:
                pass
        wait = min(reconnect_delay, 300) + secrets.randbelow(1000) / 1000.0
        print(f"Nový pokus o připojení za {wait:.1f} s...")
        time.sleep(wait)
        reconnect_delay = min(reconnect_delay * 2, 300)

if __name__ == "__main__":
    main()
