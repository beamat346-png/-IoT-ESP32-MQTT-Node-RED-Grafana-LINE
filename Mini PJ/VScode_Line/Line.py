import os
import threading
import time
from datetime import datetime, timedelta, timezone

import requests
from dotenv import load_dotenv
from flask import Flask, jsonify, request

load_dotenv()

# ====== ตั้งค่า (อ่านจาก .env) ======
LINE_CHANNEL_TOKEN = os.environ.get("LINE_CHANNEL_TOKEN", "...")
LINE_USER_ID = os.environ.get("LINE_USER_ID", "...")

# แจ้งเตือนเรื่องเดิมของโหนดเดิมซ้ำได้ไม่เกิน 1 ครั้งต่อกี่วินาที (ค่าเริ่มต้น 1800 วิ = 30 นาที)
COOLDOWN_SECONDS = int(os.environ.get("COOLDOWN_SECONDS", "1800"))
PORT = int(os.environ.get("PORT", "5000"))

# ถ้าส่ง LINE ไม่สำเร็จ จะลองใหม่ได้อีกครั้งหลังจากกี่วินาที (กันยิง API รัว ๆ)
RETRY_AFTER_FAIL_SECONDS = 60

# เกณฑ์ที่ต้องการแจ้งเตือน (ปรับตามต้องการ)
THRESHOLDS = {
    "temperature": {"min": 15.0, "max": 35.0, "unit": "°C", "label": "อุณหภูมิ", "icon": "🌡️"},
    "humidity":    {"min": 30.0, "max": 80.0, "unit": "%",  "label": "ความชื้น", "icon": "💧"},
}
# ====================================

TZ_BANGKOK = timezone(timedelta(hours=7))

app = Flask(__name__)
last_sent = {}          # (node_id, metric) -> เวลาที่แจ้งเตือนล่าสุด
state_lock = threading.Lock()


def send_line(message: str) -> bool:
    """ส่งข้อความ push เข้า LINE คืนค่า True ถ้าสำเร็จ"""
    try:
        resp = requests.post(
            "https://api.line.me/v2/bot/message/push",
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {LINE_CHANNEL_TOKEN}",
            },
            json={"to": LINE_USER_ID, "messages": [{"type": "text", "text": message}]},
            timeout=10,
        )
    except requests.RequestException as e:
        print("LINE error (เชื่อมต่อไม่ได้):", e)
        return False

    if resp.status_code != 200:
        print("LINE error:", resp.status_code, resp.text)
        return False
    return True


def build_message(node_id: str, data: dict, alert_lines: list) -> str:
    now = datetime.now(TZ_BANGKOK).strftime("%d/%m/%Y %H:%M:%S")
    parts = [f"⚠️ แจ้งเตือนจาก {node_id}", f"🕒 {now}", ""] + alert_lines

    latest = []
    for key, rule in THRESHOLDS.items():
        if key in data:
            latest.append(f"{rule['label']} {float(data[key]):.1f}{rule['unit']}")
    if latest:
        parts += ["", "ค่าล่าสุด: " + " | ".join(latest)]
    return "\n".join(parts)


def check_and_alert(data: dict) -> dict:
    node_id = str(data.get("node_id", "unknown"))
    now = time.time()
    pending = []  # [(cooldown_key, ข้อความบรรทัดแจ้งเตือน)]

    with state_lock:
        for key, rule in THRESHOLDS.items():
            if key not in data:
                continue
            value = float(data[key])  # ถ้าแปลงไม่ได้ จะ raise ValueError

            if value > rule["max"]:
                problem = f"สูงเกินกำหนด (> {rule['max']}{rule['unit']})"
            elif value < rule["min"]:
                problem = f"ต่ำกว่ากำหนด (< {rule['min']}{rule['unit']})"
            else:
                continue

            cooldown_key = (node_id, key)
            if now - last_sent.get(cooldown_key, 0) < COOLDOWN_SECONDS:
                continue  # ยังอยู่ในช่วง cooldown

            pending.append((cooldown_key, f"{rule['icon']} {rule['label']} {value:.1f}{rule['unit']} {problem}"))

        if not pending:
            return {"alerts": [], "sent": False}

        # จองช่วง cooldown ไว้ก่อน กันข้อมูลที่เข้ามาพร้อมกันส่งซ้ำ
        for cooldown_key, _ in pending:
            last_sent[cooldown_key] = now

    lines = [line for _, line in pending]
    ok = send_line(build_message(node_id, data, lines))

    if not ok:
        # ส่งไม่สำเร็จ: ปล่อยให้ลองใหม่ได้ในอีกไม่นาน แทนที่จะเงียบไปทั้งช่วง cooldown
        with state_lock:
            for cooldown_key, _ in pending:
                last_sent[cooldown_key] = now - COOLDOWN_SECONDS + RETRY_AFTER_FAIL_SECONDS

    return {"alerts": lines, "sent": ok}


@app.route("/data", methods=["POST"])
def receive_data():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"error": "ต้องส่ง JSON object"}), 400

    print("รับข้อมูล:", data)
    try:
        result = check_and_alert(data)
    except (TypeError, ValueError):
        return jsonify({"error": "temperature/humidity ต้องเป็นตัวเลข"}), 400

    return jsonify({"status": "ok", **result})


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "cooldown_seconds": COOLDOWN_SECONDS})


if __name__ == "__main__":
    if not LINE_CHANNEL_TOKEN or not LINE_USER_ID:
        raise SystemExit("กรุณาตั้งค่า LINE_CHANNEL_TOKEN และ LINE_USER_ID ในไฟล์ .env")
    app.run(host="0.0.0.0", port=PORT)