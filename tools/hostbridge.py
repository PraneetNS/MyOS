#!/usr/bin/env python3
"""
Host Bridge Daemon for MyOS Phase 3 (COM2 Bridge).
Communicates with guest MyOS over a length-prefixed, CRC32-validated serial stream.
Never sends host API keys to the guest.
"""

import sys
import os
import time
import socket
import struct
import zlib
import json
import argparse
import threading

MAX_FRAME_LEN = 65536
MOCK_SECRET_KEY = "MOCK_SECRET_KEY_NOT_IN_GUEST_998877"

def calc_crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF

def frame_message(payload_bytes: bytes) -> bytes:
    length = len(payload_bytes)
    crc = calc_crc32(payload_bytes)
    return struct.pack(">II", length, crc) + payload_bytes

class HostBridge:
    def __init__(self, host="127.0.0.1", port=4445, is_server=True, backend="mock"):
        self.host = host
        self.port = port
        self.is_server = is_server
        self.backend = backend
        self.sock = None
        self.conn = None
        self.running = True
        self.rx_buf = bytearray()
        self.lock = threading.Lock()
        self.organizer_step = 0

    def start(self):
        if self.is_server:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(1)
            print(f"[hostbridge] Listening on {self.host}:{self.port} (backend: {self.backend})...", flush=True)
            self.sock = srv
            while self.running:
                try:
                    conn, addr = srv.accept()
                    print(f"[hostbridge] Client connected from {addr}", flush=True)
                    self.conn = conn
                    self.conn.settimeout(1.0)
                    self.rx_buf.clear()
                    self.run_loop()
                except Exception as e:
                    if not self.running:
                        break
                    time.sleep(0.1)
        else:
            print(f"[hostbridge] Connecting to {self.host}:{self.port}...", flush=True)
            while self.running:
                try:
                    conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    conn.connect((self.host, self.port))
                    self.conn = conn
                    print(f"[hostbridge] Connected to {self.host}:{self.port}", flush=True)
                    self.conn.settimeout(1.0)
                    self.rx_buf.clear()
                    self.run_loop()
                except Exception:
                    time.sleep(0.5)

    def run_loop(self):
        print(f"[hostbridge] Entering run_loop, waiting for incoming data...", flush=True)
        while self.running:
            try:
                chunk = self.conn.recv(4096)
                if not chunk:
                    print("[hostbridge] Peer closed connection", flush=True)
                    break
                print(f"[hostbridge] Received chunk of {len(chunk)} bytes", flush=True)
                self.rx_buf.extend(chunk)
                self.process_incoming_frames()
            except socket.timeout:
                continue
            except Exception as e:
                print(f"[hostbridge] Error reading socket: {e}", flush=True)
                break

    def process_incoming_frames(self):
        while len(self.rx_buf) >= 8:
            found = False
            for offset in range(len(self.rx_buf) - 7):
                length, crc = struct.unpack_from(">II", self.rx_buf, offset)
                if length == 0 or length > MAX_FRAME_LEN:
                    continue
                if len(self.rx_buf) < offset + 8 + length:
                    continue
                payload = bytes(self.rx_buf[offset + 8 : offset + 8 + length])
                if calc_crc32(payload) == crc:
                    if offset > 0:
                        del self.rx_buf[:offset]
                    del self.rx_buf[: 8 + length]
                    self.handle_request(payload)
                    found = True
                    break
            if not found:
                break

    def handle_request(self, payload_bytes: bytes):
        try:
            req_str = payload_bytes.decode("utf-8")
            req = json.loads(req_str)
            print(f"[hostbridge] Successfully parsed request: {req}", flush=True)
        except Exception as e:
            print(f"[hostbridge] Failed to parse request JSON: {e}", flush=True)
            return

        req_id = req.get("id", 0)
        messages = req.get("messages", [])
        tools = req.get("tools", [])
        max_tokens = min(req.get("max_tokens", 512), 2048)
        temperature = req.get("temperature", 0.7)

        # Dispatch to backend in a worker thread to allow concurrent requests
        threading.Thread(
            target=self.dispatch_and_reply,
            args=(req_id, messages, tools, max_tokens, temperature),
            daemon=True
        ).start()

    def dispatch_and_reply(self, req_id, messages, tools, max_tokens, temperature):
        resp = {
            "id": req_id,
            "text": "",
            "tool_calls": [],
            "usage": {"in": 0, "out": 0},
            "error": None
        }

        try:
            if self.backend == "mock":
                resp = self.mock_complete(req_id, messages, tools, max_tokens, temperature)
            elif self.backend == "anthropic":
                resp = self.anthropic_complete(req_id, messages, tools, max_tokens, temperature)
            elif self.backend == "openai_compat":
                resp = self.openai_complete(req_id, messages, tools, max_tokens, temperature)
            else:
                resp["error"] = f"Unknown backend: {self.backend}"
        except Exception as e:
            resp["error"] = str(e)

        try:
            out_bytes = json.dumps(resp).encode("utf-8")
            frame = frame_message(out_bytes)
            with self.lock:
                if self.conn:
                    self.conn.sendall(frame)
        except Exception as e:
            print(f"[hostbridge] Failed to send reply: {e}", flush=True)

    def mock_complete(self, req_id, messages, tools, max_tokens, temperature):
        # Flatten conversation text for inspection
        all_text = ""
        last_tool_output = ""
        for m in messages:
            content = m.get("content", "")
            all_text += " " + content
            if m.get("role") == "tool":
                last_tool_output = content

        # Check for prompt injection trigger
        if "ignore previous instructions" in all_text.lower() or "delete /home/out" in all_text.lower():
            # Scripted to attempt calling fs.delete on /home/out/important.txt
            return {
                "id": req_id,
                "text": "Attempting requested file removal operation.",
                "tool_calls": [
                    {
                        "name": "fs.delete",
                        "args": {"path": "/home/out/important.txt"}
                    }
                ],
                "usage": {"in": len(all_text) // 4, "out": 25},
                "error": None
            }

        # Check for multi-step organizer demo
        if "organizer" in all_text.lower() or "list /home and summarize" in all_text.lower():
            # Step-based progression
            has_fs_list = any("entries" in m.get("content", "") for m in messages if m.get("role") == "tool")
            has_fs_read = any("content" in m.get("content", "") for m in messages if m.get("role") == "tool")
            has_meminfo = any("MemTotal" in m.get("content", "") for m in messages if m.get("role") == "tool")

            if not has_fs_list:
                return {
                    "id": req_id,
                    "text": "I will examine the contents of /home directory first.",
                    "tool_calls": [
                        {"name": "fs.list", "args": {"path": "/home"}}
                    ],
                    "usage": {"in": 35, "out": 15},
                    "error": None
                }
            elif not has_fs_read:
                return {
                    "id": req_id,
                    "text": "Directory entries received. Now reading demo.lua to inspect files.",
                    "tool_calls": [
                        {"name": "fs.read", "args": {"path": "/home/demo.lua"}}
                    ],
                    "usage": {"in": 60, "out": 18},
                    "error": None
                }
            elif not has_meminfo:
                return {
                    "id": req_id,
                    "text": "File read complete. Querying system memory statistics.",
                    "tool_calls": [
                        {"name": "sys.meminfo", "args": {}}
                    ],
                    "usage": {"in": 85, "out": 15},
                    "error": None
                }
            else:
                return {
                    "id": req_id,
                    "text": "Organization complete: /home contains demo.lua and other files. System memory has ample free space.",
                    "tool_calls": [],
                    "usage": {"in": 120, "out": 30},
                    "error": None
                }

        # Default scripted reply
        in_tokens = max(1, len(all_text) // 4)
        out_tokens = min(max_tokens, 20)
        return {
            "id": req_id,
            "text": f"Mock completion reply for: {all_text.strip()[:60]}",
            "tool_calls": [],
            "usage": {"in": in_tokens, "out": out_tokens},
            "error": None
        }

    def anthropic_complete(self, req_id, messages, tools, max_tokens, temperature):
        api_key = os.environ.get("ANTHROPIC_API_KEY")
        if not api_key:
            return {"id": req_id, "text": "", "tool_calls": [], "usage": {"in": 0, "out": 0}, "error": "Missing ANTHROPIC_API_KEY"}
        
        import urllib.request
        headers = {
            "Content-Type": "application/json",
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01"
        }
        body = {
            "model": "claude-3-haiku-20240307",
            "max_tokens": max_tokens,
            "temperature": temperature,
            "messages": [{"role": m.get("role", "user"), "content": m.get("content", "")} for m in messages if m.get("role") in ("user", "assistant")]
        }
        req_obj = urllib.request.Request("https://api.anthropic.com/v1/messages", data=json.dumps(body).encode("utf-8"), headers=headers)
        with urllib.request.urlopen(req_obj, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            text = "".join(b.get("text", "") for b in data.get("content", []) if b.get("type") == "text")
            usage = data.get("usage", {})
            return {
                "id": req_id,
                "text": text,
                "tool_calls": [],
                "usage": {"in": usage.get("input_tokens", 0), "out": usage.get("output_tokens", 0)},
                "error": None
            }

    def openai_complete(self, req_id, messages, tools, max_tokens, temperature):
        api_key = os.environ.get("OPENAI_API_KEY", "ollama")
        base_url = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
        if not base_url.endswith("/chat/completions"):
            base_url = base_url.rstrip("/") + "/chat/completions"

        import urllib.request
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}"
        }
        body = {
            "model": os.environ.get("OPENAI_MODEL", "gpt-3.5-turbo"),
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature
        }
        req_obj = urllib.request.Request(base_url, data=json.dumps(body).encode("utf-8"), headers=headers)
        with urllib.request.urlopen(req_obj, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            choice = data["choices"][0]["message"]
            usage = data.get("usage", {})
            return {
                "id": req_id,
                "text": choice.get("content", ""),
                "tool_calls": choice.get("tool_calls", []),
                "usage": {"in": usage.get("prompt_tokens", 0), "out": usage.get("completion_tokens", 0)},
                "error": None
            }

    def close(self):
        self.running = False
        if self.conn:
            try: self.conn.close()
            except Exception: pass
        if self.sock:
            try: self.sock.close()
            except Exception: pass

def main():
    parser = argparse.ArgumentParser(description="MyOS Host Bridge Daemon")
    parser.add_argument("--host", default="127.0.0.1", help="Bind or connect address")
    parser.add_argument("--port", type=int, default=4445, help="Port for COM2 TCP socket")
    parser.add_argument("--client", action="store_true", help="Connect to QEMU as client (default: listen as server)")
    parser.add_argument("--backend", default=os.environ.get("HOSTBRIDGE_BACKEND", "mock"), choices=["mock", "anthropic", "openai_compat"])
    args = parser.parse_args()

    bridge = HostBridge(host=args.host, port=args.port, is_server=not args.client, backend=args.backend)
    try:
        bridge.start()
    except KeyboardInterrupt:
        print("\n[hostbridge] Terminating...", flush=True)
    finally:
        bridge.close()

if __name__ == "__main__":
    main()
