#!/usr/bin/env python3
"""Снимок с ESP32-CAM -> Ollama (llava) -> MQTT smartcut/product."""
import argparse
import base64
import json
import os
import sys
import time
import urllib.request

CAM_URL = os.environ.get("CAM_URL", "http://smartcut-cam.local")
OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "llava")
MQTT_HOST = os.environ.get("MQTT_HOST", "broker.hivemq.com")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
DEVICE_ID = os.environ.get("DEVICE_ID", "").strip().strip("/")
INTERVAL = float(os.environ.get("INTERVAL", "4"))

TOPIC = f"smartcut/{DEVICE_ID}/product" if DEVICE_ID else "smartcut/product"

PRODUCTS = {
    "cucumber": "Огурец", "carrot": "Морковь", "cabbage": "Капуста", "beet": "Свёкла",
    "potato": "Картофель", "tomato": "Помидор", "onion": "Лук", "pepper": "Перец",
    "zucchini": "Кабачок", "apple": "Яблоко", "lemon": "Лимон", "eggplant": "Баклажан",
    "radish": "Редис", "garlic": "Чеснок", "mushroom": "Гриб", "orange": "Апельсин",
    "banana": "Банан", "pear": "Груша",
}
PROMPT = (
    "You see a conveyor belt from above. Which ONE food item is on it? "
    "Answer with exactly one word from this list: " + ", ".join(PRODUCTS) + ", none. "
    "If the belt is empty or the item is not in the list, answer none."
)


def http_get(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def recognize(jpeg):
    body = json.dumps({
        "model": OLLAMA_MODEL, "prompt": PROMPT, "stream": False,
        "images": [base64.b64encode(jpeg).decode()],
        "options": {"temperature": 0},
    }).encode()
    req = urllib.request.Request(f"{OLLAMA_URL}/api/generate", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        answer = json.load(r).get("response", "").lower()
    for word, ru in PRODUCTS.items():
        if word in answer:
            return ru
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", help="распознать файл и выйти")
    args = ap.parse_args()

    if args.image:
        print(recognize(open(args.image, "rb").read()) or "ничего не найдено")
        return

    import paho.mqtt.client as mqtt
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()
    client.connect(MQTT_HOST, MQTT_PORT, 30)
    client.loop_start()
    print(f"Камера {CAM_URL}, Ollama {OLLAMA_URL} ({OLLAMA_MODEL}), топик {TOPIC}")

    last, last_pub = None, 0.0
    while True:
        try:
            product = recognize(http_get(f"{CAM_URL}/capture?led=1"))
        except Exception as e:
            print("ошибка:", e, file=sys.stderr)
            time.sleep(INTERVAL)
            continue
        if product != last or time.time() - last_pub > 20:
            client.publish(TOPIC, json.dumps({"product": product or "none"}, ensure_ascii=False), retain=True)
            last, last_pub = product, time.time()
            print("на ленте:", product or "ничего")
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
