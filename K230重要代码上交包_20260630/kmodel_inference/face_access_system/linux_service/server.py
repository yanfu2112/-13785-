#!/usr/bin/env python3
import csv
import json
import os
import subprocess
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(os.environ.get("FACE_ACCESS_ROOT", "/sharefs/face_access"))
LOG_PATH = ROOT / "access.csv"
COMMAND_PATH = ROOT / "command.txt"
WEBHOOK_URL = os.environ.get("FACE_ACCESS_WEBHOOK", "")
PORT = int(os.environ.get("FACE_ACCESS_PORT", "8080"))
ADB_MODE = os.environ.get("FACE_ACCESS_ADB", "").lower() in {"1", "true", "yes"}
ADB_BIN = os.environ.get("FACE_ACCESS_ADB_BIN", "adb")


INDEX_HTML = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>K230 人脸门禁</title><style>
body{font-family:system-ui,sans-serif;background:#f3f6fa;color:#172033;margin:0}.wrap{max-width:1000px;margin:32px auto;padding:0 18px}
.card{background:white;border-radius:14px;padding:20px;box-shadow:0 6px 24px #19324d14;margin-bottom:18px}h1{margin:0 0 8px}
input,button{padding:10px 12px;border-radius:8px;border:1px solid #ccd5df}button{background:#1769e0;color:white;border:0;cursor:pointer}
button.danger{background:#b3261e}table{width:100%;border-collapse:collapse;font-size:14px}th,td{padding:9px;border-bottom:1px solid #e8edf3;text-align:left}
.ok{color:#087f3e}.alarm{color:#b3261e;font-weight:700}</style></head>
<body><main class="wrap"><section class="card"><h1>K230 人脸门禁</h1><div id="health">正在连接…</div></section>
<section class="card"><h2>人员管理</h2><input id="name" maxlength="31" placeholder="输入姓名"><button onclick="reg()">下一张正脸注册</button>
<button class="danger" onclick="resetDb()">清空人脸库</button></section>
<section class="card"><h2>最近事件</h2><table><thead><tr><th>时间</th><th>姓名</th><th>相似度</th><th>延迟</th><th>动作</th></tr></thead><tbody id="rows"></tbody></table></section></main>
<script>
async function api(path,opt){const r=await fetch(path,opt);if(!r.ok)throw Error(await r.text());return r.json()}
const healthEl=document.querySelector('#health');
async function refresh(){try{const h=await api('/api/health');healthEl.textContent=`服务正常 · 日志 ${h.log_exists?'可用':'等待生成'}`;
const rows=await api('/api/logs');document.querySelector('#rows').innerHTML=rows.map(x=>`<tr><td>${x.monotonic_ms||''}</td><td>${x.name||''}</td><td>${x.similarity||''}</td><td>${x.latency_ms||''} ms</td><td class="${x.action==='ALARM'?'alarm':'ok'}">${x.action||''}</td></tr>`).join('')}catch(e){healthEl.textContent='连接失败：'+e}}
async function reg(){const name=document.querySelector('#name').value.trim();if(!name)return alert('请输入姓名');await api('/api/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name})});alert('请正视摄像头，等待注册')}
async function resetDb(){if(confirm('确定清空全部人脸？'))await api('/api/reset',{method:'POST'})}setInterval(refresh,2000);refresh();
</script></body></html>"""


def adb_run(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run([ADB_BIN, *args], check=check, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def remote_file_exists(path: Path) -> bool:
    if not ADB_MODE:
        return path.exists()
    return adb_run("shell", "test", "-f", str(path), check=False).returncode == 0


def write_command(command: str) -> None:
    if ADB_MODE:
        temporary_name = None
        try:
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n",
                                             delete=False) as stream:
                stream.write(command + "\n")
                temporary_name = stream.name
            adb_run("push", temporary_name, str(COMMAND_PATH))
        finally:
            if temporary_name:
                Path(temporary_name).unlink(missing_ok=True)
        return
    ROOT.mkdir(parents=True, exist_ok=True)
    temporary = COMMAND_PATH.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(command + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, COMMAND_PATH)


def read_logs(limit: int = 100):
    if ADB_MODE:
        result = adb_run("shell", "cat", str(LOG_PATH), check=False)
        if result.returncode != 0:
            return []
        rows = list(csv.DictReader(result.stdout.splitlines()))
    else:
        if not LOG_PATH.exists():
            return []
        with LOG_PATH.open("r", encoding="utf-8", errors="replace", newline="") as stream:
            rows = list(csv.DictReader(stream))
    return list(reversed(rows[-limit:]))


class Handler(BaseHTTPRequestHandler):
    def send_json(self, payload, status=200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            body = INDEX_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/health":
            adb_connected = True
            if ADB_MODE:
                adb_connected = adb_run("get-state", check=False).stdout.strip() == "device"
            self.send_json({"ok": adb_connected,
                            "log_exists": remote_file_exists(LOG_PATH),
                            "root": str(ROOT), "mode": "adb" if ADB_MODE else "local"})
        elif self.path.startswith("/api/logs"):
            self.send_json(read_logs())
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        if self.path == "/api/register":
            length = int(self.headers.get("Content-Length", "0"))
            try:
                payload = json.loads(self.rfile.read(length) or b"{}")
                name = str(payload.get("name", "")).strip()
            except (ValueError, UnicodeDecodeError):
                return self.send_json({"error": "invalid json"}, 400)
            if not name or len(name.encode("utf-8")) >= 32 or "\n" in name or ":" in name:
                return self.send_json({"error": "invalid name"}, 400)
            write_command("REGISTER:" + name)
            self.send_json({"ok": True})
        elif self.path == "/api/reset":
            write_command("RESET")
            self.send_json({"ok": True})
        else:
            self.send_json({"error": "not found"}, 404)

    def log_message(self, fmt, *args):
        print("[web] " + fmt % args)


def alarm_watcher():
    seen = set()
    while True:
        try:
            for row in read_logs(200):
                key = (row.get("monotonic_ms"), row.get("frame_id"), row.get("action"))
                if key in seen:
                    continue
                seen.add(key)
                if WEBHOOK_URL and row.get("action") == "ALARM":
                    body = json.dumps(row, ensure_ascii=False).encode("utf-8")
                    request = urllib.request.Request(WEBHOOK_URL, data=body,
                                                     headers={"Content-Type": "application/json"})
                    urllib.request.urlopen(request, timeout=5).read()
            if len(seen) > 1000:
                seen.clear()
        except Exception as exc:
            print("[watcher]", exc)
        time.sleep(2)


if __name__ == "__main__":
    if not ADB_MODE:
        ROOT.mkdir(parents=True, exist_ok=True)
    threading.Thread(target=alarm_watcher, daemon=True).start()
    print(f"Face access web ({'adb' if ADB_MODE else 'local'}): http://0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
