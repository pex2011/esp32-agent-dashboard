#!/usr/bin/env python3
"""
Server locale per il display ESP32 (Cheap Yellow Display).

Espone le sessioni Claude Code vive su questo Mac (con contesto, token, turni,
ultimo prompt letti dai transcript) e, se abilitato, i consumi di Claude e Codex.

  python3 dash_server.py                                   # HTTP su 0.0.0.0:8765 -> GET /api/state
  python3 dash_server.py --serial /dev/cu.usbserial-XXXX   # in più invia lo stato via USB ogni N secondi
  python3 dash_server.py --no-usage                        # non legge Keychain / auth Codex
  python3 dash_server.py --once                            # stampa lo stato e esce

Azioni dal display:  POST /api/focus?pid=N  (o riga seriale "ACT focus N") -> apre la cartella in VS Code.
Solo libreria standard (pyserial solo per --serial). I token non vengono mai stampati né salvati.
"""
import argparse, glob, json, os, re, subprocess, threading, time, urllib.request, ssl, socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HOME = os.path.expanduser("~")
SESS_DIR = os.path.join(HOME, ".claude", "sessions")
PROJ_DIR = os.path.join(HOME, ".claude", "projects")
CTX = ssl.create_default_context(cafile="/etc/ssl/cert.pem" if os.path.exists("/etc/ssl/cert.pem") else None)
TYPE_RE = re.compile(rb'"type":\s*"(assistant|user)"')

# ---------------------------------------------------------------- transcript (incrementale, append-only)
def short_model(m):
    if not m: return ""
    parts = m.replace("claude-", "").split("-")
    nums = [p for p in parts[1:] if p.isdigit() and len(p) <= 2]
    return (parts[0] + " " + ".".join(nums[:2])).strip()

class Transcript:
    _c = {}
    @classmethod
    def info(cls, cwd, sid):
        if not sid or not cwd: return {}
        path = os.path.join(PROJ_DIR, re.sub(r"[^A-Za-z0-9-]", "-", cwd), sid + ".jsonl")
        try: size = os.path.getsize(path)
        except OSError: return {}
        st = cls._c.get(path)
        if st is None or size < st["off"]:
            st = {"off": 0, "out": 0, "cr": 0, "turns": 0, "ctx": 0, "model": "", "last": "", "tool": "", "usage_by_id": {}}
            cls._c[path] = st
        if size > st["off"] and size < 80_000_000:
            with open(path, "rb") as f:
                f.seek(st["off"])
                for raw in f:
                    if not raw.endswith(b"\n"): break       # riga ancora in scrittura
                    st["off"] += len(raw)
                    cls._feed(st, raw)
        ctxk = st["ctx"] // 1000
        win = 1000 if ctxk > 200 else 200
        return {"ctx": min(100, round(ctxk * 100 / win)), "ctxk": ctxk, "win": win, "out": st["out"], "cr": st["cr"],
                "turns": st["turns"], "model": short_model(st["model"]), "last": st["last"], "tool": st["tool"]}

    @staticmethod
    def _feed(st, raw):
        m = TYPE_RE.search(raw)
        if not m: return
        try: d = json.loads(raw)
        except Exception: return
        msg = d.get("message") or {}
        if m.group(1) == b"assistant":
            u = msg.get("usage") or {}
            if u:
                inp = (u.get("input_tokens") or 0) + (u.get("cache_read_input_tokens") or 0) + (u.get("cache_creation_input_tokens") or 0)
                out, cr = u.get("output_tokens") or 0, u.get("cache_read_input_tokens") or 0
                if inp: st["ctx"] = inp  # righe sintetiche a zero non cancellano l'ultimo contesto valido
                key = msg.get("id") or d.get("uuid") or st["off"]
                old_out, old_cr = st["usage_by_id"].get(key, (0, 0))
                st["out"] += max(0, out - old_out)
                st["cr"] += max(0, cr - old_cr)
                st["usage_by_id"][key] = (max(out, old_out), max(cr, old_cr))
            if msg.get("model") and not msg["model"].startswith("<"): st["model"] = msg["model"]
            for part in (msg.get("content") or []):
                if isinstance(part, dict) and part.get("type") == "tool_use":
                    st["tool"] = part.get("name") or st["tool"]
        else:
            if d.get("isMeta"): return
            c = msg.get("content"); text = None
            if isinstance(c, str): text = c
            elif isinstance(c, list):
                text = next((p.get("text") for p in c if isinstance(p, dict) and p.get("type") == "text"), None)
            if text:
                t = text.strip()
                if t and not t.startswith("<"):
                    st["turns"] += 1; st["last"] = re.sub(r"\s+", " ", t)[:60]

# ---------------------------------------------------------------- sessioni
def pid_alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True

def read_sessions(with_transcript=True):
    now = time.time(); out = []
    for f in glob.glob(os.path.join(SESS_DIR, "*.json")):
        try: d = json.load(open(f))
        except Exception: continue
        pid = d.get("pid")
        if not pid or not pid_alive(pid): continue
        cwd = d.get("cwd") or ""
        s = {
            "pid": pid,
            "name": d.get("name") or os.path.basename(cwd) or str(pid),
            "proj": os.path.basename(cwd.rstrip("/")) or cwd,
            "cwd": cwd.replace(HOME, "~"),
            "st": d.get("status") or "?",                                   # busy | idle
            "kind": "sdk" if (d.get("entrypoint") == "sdk-cli" or ".claude-mem" in cwd) else ("ide" if d.get("entrypoint") == "claude-vscode" else "cli"),
            "ver": d.get("version"),
            "age": int(now - d.get("startedAt", now * 1000) / 1000),
            "idle": int(now - d.get("updatedAt", now * 1000) / 1000),
        }
        if with_transcript:
            try: s.update(Transcript.info(cwd, d.get("sessionId")))
            except Exception as e: s["terr"] = str(e)[:60]
        out.append(s)
    out.sort(key=lambda s: (s["kind"] == "sdk", s["st"] != "busy", s["idle"]))
    return out

def count_procs(pattern):
    try:
        r = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True, timeout=5)
        return len(r.stdout.split())
    except Exception: return None

def focus(pid):
    for s in read_sessions(with_transcript=False):
        if s["pid"] == pid:
            cwd = s["cwd"].replace("~", HOME, 1)
            if s["kind"] == "ide": subprocess.Popen(["open", "-a", "Visual Studio Code", cwd])
            else: subprocess.Popen(["open", "-a", "Terminal"])
            return True
    return False

# ---------------------------------------------------------------- consumi (best effort)
def http_json(url, headers, timeout=15):
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=timeout, context=CTX) as r:
        return json.load(r)

def claude_usage():
    raw = subprocess.run(["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
                         capture_output=True, text=True, timeout=60).stdout.strip()
    oauth = json.loads(raw).get("claudeAiOauth", {})
    tok = oauth.get("accessToken")
    if not tok: raise RuntimeError("token non trovato nel Keychain")
    d = http_json("https://api.anthropic.com/api/oauth/usage",
                  {"Authorization": f"Bearer {tok}", "anthropic-beta": "oauth-2025-04-20"})
    res = {"plan": oauth.get("subscriptionType")}
    for k, v in d.items():
        if isinstance(v, dict) and "utilization" in v:
            res[k] = {"pct": round(float(v.get("utilization") or 0)), "reset": v.get("resets_at")}
    return res

def codex_usage():
    d = json.load(open(os.path.join(HOME, ".codex", "auth.json")))
    t = d.get("tokens", {}); tok, acc = t.get("access_token"), t.get("account_id", "")
    if not tok: raise RuntimeError("token Codex non trovato")
    u = http_json("https://chatgpt.com/backend-api/wham/usage",
                  {"Authorization": f"Bearer {tok}", "ChatGPT-Account-Id": acc, "User-Agent": "codex_cli_rs"})
    res = {"plan": u.get("plan_type")}
    rl = u.get("rate_limit") or u.get("rate_limits") or {}
    for key, label in (("primary_window", "five_hour"), ("secondary_window", "weekly")):
        w = rl.get(key)
        if isinstance(w, dict):
            res[label] = {"pct": round(float(w.get("used_percent") or 0)), "reset_in": w.get("reset_after_seconds")}
    if "five_hour" not in res:
        def walk(o, path=""):
            if isinstance(o, dict):
                if "used_percent" in o: res[path.strip(".") or "window"] = {"pct": round(float(o["used_percent"] or 0))}
                for k, v in o.items(): walk(v, path + "." + k)
        walk(u)
    return res

class UsageCache:
    def __init__(self, enabled, every=120):
        self.data = {"claude": None, "codex": None, "err": {}}; self.lock = threading.Lock(); self.every = every
        if enabled: threading.Thread(target=self._loop, daemon=True).start()
        else: self.data["err"] = {"claude": "disabilitato (--no-usage)", "codex": "disabilitato (--no-usage)"}
    def _loop(self):
        while True:
            for name, fn in (("claude", claude_usage), ("codex", codex_usage)):
                try: v, err = fn(), None
                except Exception as e: v, err = None, f"{type(e).__name__}: {str(e)[:100]}"
                with self.lock:
                    if v is not None: self.data[name] = v
                    if err: self.data["err"][name] = err
                    else: self.data["err"].pop(name, None)
            time.sleep(self.every)
    def get(self):
        with self.lock: return json.loads(json.dumps(self.data))

# ---------------------------------------------------------------- stato
def build_state(usage, max_sessions=16):
    s = read_sessions()
    return {"ts": int(time.time()), "clock": time.strftime("%H:%M"), "host": socket.gethostname().split(".")[0],
            "n": len(s), "busy": sum(1 for x in s if x["st"] == "busy"),
            "codex_procs": count_procs("codex-code-mode-host|/codex -c"),
            "sessions": s[:max_sessions], "usage": usage.get()}

class Handler(BaseHTTPRequestHandler):
    usage = None
    def log_message(self, *a): pass
    def _send(self, code, body, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def _route(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/api/state":
            self._send(200, json.dumps(build_state(self.usage), separators=(",", ":")).encode())
        elif u.path == "/api/focus":
            ok = focus(int(q.get("pid", ["0"])[0])); self._send(200 if ok else 404, json.dumps({"ok": ok}).encode())
        elif u.path == "/":
            self._send(200, b"<pre>ESP32 dash server\nGET  /api/state\nPOST /api/focus?pid=N</pre>", "text/html")
        else: self._send(404, b"{}")
    def do_GET(self): self._route()
    def do_POST(self): self._route()

def serial_loop(port, usage, interval):
    import serial  # pyserial
    while True:
        try:
            sp = serial.Serial(port, 115200, timeout=0.5)
            sp.dtr = False; sp.rts = False
            print(f"[serial] connesso a {port}")
            def reader():
                while True:
                    try: line = sp.readline().decode("utf-8", "replace").strip()
                    except Exception: return
                    if not line: continue
                    if line.startswith("ACT focus "):
                        try: print(f"[serial] focus pid {line.split()[2]} ->", focus(int(line.split()[2])))
                        except Exception as e: print("[serial] focus err", e)
                    else: print(f"[esp32] {line}")
            threading.Thread(target=reader, daemon=True).start()
            while True:
                sp.write((json.dumps(build_state(usage, 16), separators=(",", ":")) + "\n").encode())
                time.sleep(interval)
        except Exception as e:
            print(f"[serial] {e} - riprovo tra 3 s"); time.sleep(3)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--serial", help="porta seriale della scheda, es. /dev/cu.usbserial-2011230")
    ap.add_argument("--interval", type=float, default=3.0)
    ap.add_argument("--no-usage", action="store_true")
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()
    usage = UsageCache(enabled=not a.no_usage)
    if a.once:
        print(json.dumps(build_state(usage), indent=1, ensure_ascii=False)); return
    Handler.usage = usage
    if a.serial:
        threading.Thread(target=serial_loop, args=(a.serial, usage, a.interval), daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    ip = subprocess.run(["ipconfig", "getifaddr", "en0"], capture_output=True, text=True).stdout.strip() or "?"
    print(f"[http] http://{ip}:{a.port}/api/state   (Ctrl+C per uscire)")
    try: srv.serve_forever()
    except KeyboardInterrupt: pass

if __name__ == "__main__":
    main()
