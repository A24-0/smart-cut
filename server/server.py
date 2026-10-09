#!/usr/bin/env python3
"""Локальный сервер рецептов для SmartCutApp."""
import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("PORT", "8081"))
HERE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(HERE, "smartcut.db")
IMAGES_DIR = os.path.join(HERE, "images")
SECRET_FILE = os.path.join(HERE, ".secret")

os.makedirs(IMAGES_DIR, exist_ok=True)
if not os.path.exists(SECRET_FILE):
    with open(SECRET_FILE, "w") as f:
        f.write(secrets.token_hex(32))
SECRET = open(SECRET_FILE).read().strip().encode()

db_lock = threading.Lock()
db = sqlite3.connect(DB_PATH, check_same_thread=False)
db.row_factory = sqlite3.Row
db.executescript("""
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,
  email TEXT UNIQUE NOT NULL, password_hash TEXT NOT NULL, salt TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS recipes (
  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, cooking_time TEXT,
  image_url TEXT, user_id INTEGER NOT NULL, servings INTEGER DEFAULT 1,
  steps TEXT, tags TEXT);
CREATE TABLE IF NOT EXISTS ingredients (
  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, amount TEXT,
  cuttable INTEGER DEFAULT 0, grams INTEGER, cut_type TEXT,
  recipe_id INTEGER NOT NULL REFERENCES recipes(id) ON DELETE CASCADE);
""")
db.execute("PRAGMA foreign_keys = ON")


def hash_password(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), 100_000).hex()


def make_token(user_id):
    msg = str(user_id)
    sig = hmac.new(SECRET, msg.encode(), hashlib.sha256).hexdigest()
    return base64.urlsafe_b64encode(f"{msg}:{sig}".encode()).decode()


def parse_token(token):
    try:
        msg, sig = base64.urlsafe_b64decode(token.encode()).decode().split(":")
        good = hmac.new(SECRET, msg.encode(), hashlib.sha256).hexdigest()
        return int(msg) if hmac.compare_digest(sig, good) else None
    except Exception:
        return None


def recipe_json(row):
    ings = db.execute("SELECT * FROM ingredients WHERE recipe_id=? ORDER BY id", (row["id"],)).fetchall()
    return {
        "id": row["id"], "name": row["name"], "cookingTime": row["cooking_time"],
        "imageUrl": row["image_url"], "userId": row["user_id"], "servings": row["servings"] or 1,
        "steps": row["steps"], "tags": row["tags"],
        "ingredients": [{
            "id": i["id"], "name": i["name"], "amount": i["amount"], "cuttable": bool(i["cuttable"]),
            "grams": i["grams"], "cutType": i["cut_type"]} for i in ings],
    }


def save_ingredients(recipe_id, items):
    db.execute("DELETE FROM ingredients WHERE recipe_id=?", (recipe_id,))
    for it in items or []:
        db.execute(
            "INSERT INTO ingredients(name, amount, cuttable, grams, cut_type, recipe_id) VALUES(?,?,?,?,?,?)",
            (it.get("name", ""), it.get("amount"), 1 if it.get("cuttable") else 0,
             it.get("grams"), it.get("cutType"), recipe_id))


def seed():
    if db.execute("SELECT COUNT(*) FROM recipes").fetchone()[0]:
        return
    recipes = [
        ("Овощной салат", "15 мин", 2, "Нарежьте овощи.\nСмешайте и заправьте маслом.", "салат,овощи",
         [("Огурец", "200 г", "sticks", 200), ("Морковь", "150 г", "grate", 150), ("Капуста", "250 г", "shred", 250),
          ("Масло", "2 ст. л.", None, None)]),
        ("Борщ", "90 мин", 4, "Нарежьте овощи.\nСварите бульон.\nДобавьте овощи и варите 30 минут.", "суп",
         [("Свёкла", "300 г", "sticks", 300), ("Капуста", "300 г", "shred", 300), ("Морковь", "150 г", "grate", 150),
          ("Картофель", "400 г", "sticks", 400), ("Мясо", "500 г", None, None)]),
        ("Картофель фри", "30 мин", 2, "Нарежьте картофель брусочками.\nОбжарьте до золотистого цвета.", "гарнир",
         [("Картофель", "500 г", "sticks", 500), ("Соль", "по вкусу", None, None)]),
    ]
    for name, time_, serv, steps, tags, ings in recipes:
        cur = db.execute(
            "INSERT INTO recipes(name, cooking_time, image_url, user_id, servings, steps, tags) VALUES(?,?,?,?,?,?,?)",
            (name, time_, None, 0, serv, steps, tags))
        save_ingredients(cur.lastrowid, [
            {"name": n, "amount": a, "cuttable": c is not None, "grams": g, "cutType": c} for n, a, c, g in ings])
    db.commit()


seed()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("%s %s" % (self.address_string(), fmt % args))

    def send_json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_text(self, status, text):
        body = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def read_json(self):
        try:
            return json.loads(self.read_body() or b"{}")
        except Exception:
            return None

    def user_id(self):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return None
        return parse_token(auth[7:])

    def do_GET(self):
        path = self.path.split("?")[0]
        if path.startswith("/images/"):
            return self.serve_image(path[len("/images/"):])
        if path == "/health":
            return self.send_json(200, {"ok": True})
        if self.user_id() is None:
            return self.send_text(401, "Unauthorized")
        with db_lock:
            if path == "/recipes":
                rows = db.execute("SELECT * FROM recipes ORDER BY id DESC").fetchall()
                return self.send_json(200, [recipe_json(r) for r in rows])
            m = re.fullmatch(r"/recipes/(\d+)", path)
            if m:
                row = db.execute("SELECT * FROM recipes WHERE id=?", (int(m.group(1)),)).fetchone()
                return self.send_json(200, recipe_json(row)) if row else self.send_text(404, "Not found")
        self.send_text(404, "Not found")

    def do_POST(self):
        path = self.path.split("?")[0]
        if path in ("/auth/register", "/auth/login"):
            return self.auth(path)
        uid = self.user_id()
        if uid is None:
            return self.send_text(401, "Unauthorized")
        if path == "/images/upload":
            return self.upload_image()
        if path == "/recipes":
            data = self.read_json()
            if not data or not data.get("name"):
                return self.send_text(400, "Bad request")
            with db_lock:
                cur = db.execute(
                    "INSERT INTO recipes(name, cooking_time, image_url, user_id, servings, steps, tags) VALUES(?,?,?,?,?,?,?)",
                    (data["name"], data.get("cookingTime"), data.get("imageUrl"), uid,
                     data.get("servings", 1), data.get("steps"), data.get("tags")))
                save_ingredients(cur.lastrowid, data.get("ingredients"))
                db.commit()
                row = db.execute("SELECT * FROM recipes WHERE id=?", (cur.lastrowid,)).fetchone()
                return self.send_json(201, recipe_json(row))
        self.send_text(404, "Not found")

    def do_PUT(self):
        uid = self.user_id()
        if uid is None:
            return self.send_text(401, "Unauthorized")
        m = re.fullmatch(r"/recipes/(\d+)", self.path.split("?")[0])
        data = self.read_json()
        if not m or not data or not data.get("name"):
            return self.send_text(400, "Bad request")
        rid = int(m.group(1))
        with db_lock:
            if not db.execute("SELECT 1 FROM recipes WHERE id=?", (rid,)).fetchone():
                return self.send_text(404, "Not found")
            db.execute(
                "UPDATE recipes SET name=?, cooking_time=?, image_url=?, servings=?, steps=?, tags=? WHERE id=?",
                (data["name"], data.get("cookingTime"), data.get("imageUrl"), data.get("servings", 1),
                 data.get("steps"), data.get("tags"), rid))
            save_ingredients(rid, data.get("ingredients"))
            db.commit()
            row = db.execute("SELECT * FROM recipes WHERE id=?", (rid,)).fetchone()
            return self.send_json(200, recipe_json(row))

    def do_DELETE(self):
        if self.user_id() is None:
            return self.send_text(401, "Unauthorized")
        m = re.fullmatch(r"/recipes/(\d+)", self.path.split("?")[0])
        if not m:
            return self.send_text(404, "Not found")
        with db_lock:
            db.execute("DELETE FROM recipes WHERE id=?", (int(m.group(1)),))
            db.commit()
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def auth(self, path):
        data = self.read_json()
        if not data:
            return self.send_text(400, "Bad request")
        email = str(data.get("email", "")).strip().lower()
        password = str(data.get("password", ""))
        with db_lock:
            if path == "/auth/register":
                name = str(data.get("name", "")).strip()
                if not name or not email or len(password) < 4:
                    return self.send_text(400, "Bad request")
                if db.execute("SELECT 1 FROM users WHERE email=?", (email,)).fetchone():
                    return self.send_text(409, "Email already registered")
                salt = secrets.token_hex(8)
                cur = db.execute("INSERT INTO users(name, email, password_hash, salt) VALUES(?,?,?,?)",
                                 (name, email, hash_password(password, salt), salt))
                db.commit()
                return self.send_json(200, {"token": make_token(cur.lastrowid), "userId": cur.lastrowid, "name": name})
            row = db.execute("SELECT * FROM users WHERE email=?", (email,)).fetchone()
            if not row or not hmac.compare_digest(row["password_hash"], hash_password(password, row["salt"])):
                return self.send_text(401, "Invalid credentials")
            return self.send_json(200, {"token": make_token(row["id"]), "userId": row["id"], "name": row["name"]})

    def upload_image(self):
        ctype = self.headers.get("Content-Type", "")
        m = re.search(r"boundary=(.+)", ctype)
        if not m:
            return self.send_text(400, "Bad request")
        boundary = ("--" + m.group(1).strip('"')).encode()
        body = self.read_body()
        for part in body.split(boundary):
            head, sep, content = part.partition(b"\r\n\r\n")
            if sep and b"filename=" in head:
                content = content.rsplit(b"\r\n", 1)[0]
                name = f"{uuid.uuid4().hex}.jpg"
                with open(os.path.join(IMAGES_DIR, name), "wb") as f:
                    f.write(content)
                host = self.headers.get("Host", f"localhost:{PORT}")
                return self.send_json(200, {"url": f"http://{host}/images/{name}"})
        self.send_text(400, "No image")

    def serve_image(self, name):
        safe = os.path.basename(name)
        full = os.path.join(IMAGES_DIR, safe)
        if not os.path.isfile(full):
            return self.send_text(404, "Not found")
        data = open(full, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    print(f"SmartCut server: http://0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
