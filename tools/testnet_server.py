#!/usr/bin/env python3
"""
tools/testnet_server.py - Host mock network server for Phase 6 hermetic tests.
Runs:
  - HTTP server on port 8080
  - HTTPS valid server on port 8443 (signed by test_ca)
  - HTTPS wrong-host server on port 8444 (CN=wrong.example.com)
  - HTTPS expired cert server on port 8445 (expired in 2021)
  - HTTPS untrusted CA server on port 8446 (signed by rogue_ca)
  - Raw TCP echo server on port 7777
  - Raw UDP echo server on port 7777
  - DNS test stub on UDP port 1053 (with DNS rebinding simulation)

Endpoints:
  - /health: 200 {"status": "ok"}
  - /echo: echoes request method, headers, and body as JSON.
  - /secret: verifies injected secret header (e.g. Authorization: Bearer <token>).
  - /header-echo: echoes all headers as X-Echo-* and in JSON body.
  - /big: returns 512 KB of data.
  - /oversized: returns 1 MB of data.
  - /redirect: HTTP 302 redirect to /echo.
  - /redirect-chain: HTTP 302 chain down to /echo.
  - /redirect-to: HTTP 302 redirect to arbitrary ?url=...
  - /endless-headers: streams endless headers slowly to trigger header limits.
  - /slow-loris: sends body 1 byte per second to trigger wall-clock timeouts.
  - /rebinding: simulated DNS rebinding target.
"""

import sys
import os
import time
import json
import ssl
import socket
import struct
import subprocess
import threading
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

HTTP_PORT = int(os.environ.get("TESTNET_HTTP_PORT", "8080"))
HTTPS_PORT = int(os.environ.get("TESTNET_HTTPS_PORT", "8443"))
HTTPS_WRONG_PORT = int(os.environ.get("TESTNET_HTTPS_WRONG_PORT", "8444"))
HTTPS_EXPIRED_PORT = int(os.environ.get("TESTNET_HTTPS_EXPIRED_PORT", "8445"))
HTTPS_UNTRUSTED_PORT = int(os.environ.get("TESTNET_HTTPS_UNTRUSTED_PORT", "8446"))
TCP_ECHO_PORT = int(os.environ.get("TESTNET_TCP_ECHO_PORT", "7777"))
UDP_ECHO_PORT = int(os.environ.get("TESTNET_UDP_ECHO_PORT", "7777"))
DNS_PORT = int(os.environ.get("TESTNET_DNS_PORT", "1053"))

CERT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "ssl")
EXPECTED_SECRET = "Bearer topsecret-test-token-12345"

s_rebind_query_count = 0
s_rebind_lock = threading.Lock()


class TestnetHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, format, *args):
        sys.stderr.write(f"[testnet] {self.command} {self.path} -> {args[0]}\n")

    def _send_json(self, status_code, data, extra_headers=None):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        self._handle_request()

    def do_POST(self):
        self._handle_request()

    def do_PUT(self):
        self._handle_request()

    def do_DELETE(self):
        self._handle_request()

    def _handle_request(self):
        try:
            content_length = int(self.headers.get("Content-Length", 0))
        except Exception:
            content_length = 0
        body = self.rfile.read(content_length) if content_length > 0 else b""

        path_clean = self.path.split("?")[0]

        if path_clean == "/health":
            self._send_json(200, {"status": "ok"})

        elif path_clean == "/echo":
            headers_dict = {k: v for k, v in self.headers.items()}
            self._send_json(200, {
                "method": self.command,
                "path": self.path,
                "headers": headers_dict,
                "body": body.decode("utf-8", errors="replace"),
                "body_len": len(body)
            })

        elif path_clean == "/secret":
            auth = self.headers.get("Authorization", "")
            if auth == EXPECTED_SECRET:
                self._send_json(200, {
                    "status": "ok",
                    "message": "secret verified",
                    "secret_verified": True,
                    "auth": auth
                })
            else:
                self._send_json(401, {
                    "status": "error",
                    "error": "unauthorized",
                    "received": auth
                })

        elif path_clean == "/header-echo":
            headers_dict = {k: v for k, v in self.headers.items()}
            extra = {}
            for k, v in self.headers.items():
                extra[f"X-Echo-{k}"] = v
            self._send_json(200, {
                "status": "ok",
                "headers": headers_dict,
                "body": body.decode("utf-8", errors="replace")
            }, extra_headers=extra)

        elif path_clean == "/big":
            chunk = b"0123456789abcdef" * 64  # 1024 bytes
            total_size = 512 * 1024
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(total_size))
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            for _ in range(total_size // len(chunk)):
                self.wfile.write(chunk)

        elif path_clean == "/oversized":
            chunk = b"0123456789abcdef" * 64  # 1024 bytes
            total_size = 1024 * 1024  # 1 MB
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(total_size))
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            for _ in range(total_size // len(chunk)):
                self.wfile.write(chunk)

        elif path_clean == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/echo")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True

        elif path_clean == "/redirect-chain":
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            hops = int(qs.get("hops", ["1"])[0])
            self.send_response(302)
            if hops > 1:
                self.send_header("Location", f"/redirect-chain?hops={hops-1}")
            else:
                self.send_header("Location", "/echo")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True

        elif path_clean == "/redirect-to":
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            target = qs.get("url", ["/echo"])[0]
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True

        elif path_clean == "/endless-headers":
            self.wfile.write(b"HTTP/1.0 200 OK\r\n")
            try:
                for i in range(1000):
                    line = f"X-Endless-Header-{i:04d}: padding-data-to-expand-header-length-{i:08d}\r\n".encode("utf-8")
                    self.wfile.write(line)
                    self.wfile.flush()
                    time.sleep(0.01)
            except Exception:
                pass
            self.close_connection = True

        elif path_clean == "/slow-loris":
            self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 100\r\n\r\n")
            self.wfile.flush()
            try:
                for _ in range(100):
                    self.wfile.write(b"X")
                    self.wfile.flush()
                    time.sleep(1.0)
            except Exception:
                pass
            self.close_connection = True

        elif path_clean == "/rebinding":
            self._send_json(200, {
                "status": "ok",
                "message": "rebinding target reachable"
            })

        else:
            self._send_json(404, {"error": "not found", "path": self.path})


def ensure_test_certificates():
    os.makedirs(CERT_DIR, exist_ok=True)
    ca_key = os.path.join(CERT_DIR, "test_ca.key")
    ca_crt = os.path.join(CERT_DIR, "test_ca.crt")
    server_key = os.path.join(CERT_DIR, "server.key")
    server_csr = os.path.join(CERT_DIR, "server.csr")
    server_crt = os.path.join(CERT_DIR, "server.crt")
    san_cnf = os.path.join(CERT_DIR, "san.cnf")

    wrong_key = os.path.join(CERT_DIR, "wronghost.key")
    wrong_csr = os.path.join(CERT_DIR, "wronghost.csr")
    wrong_crt = os.path.join(CERT_DIR, "wronghost.crt")
    wrong_cnf = os.path.join(CERT_DIR, "wrong_san.cnf")

    expired_crt = os.path.join(CERT_DIR, "expired.crt")

    rogue_key = os.path.join(CERT_DIR, "rogue_ca.key")
    rogue_crt = os.path.join(CERT_DIR, "rogue_ca.crt")
    untrusted_key = os.path.join(CERT_DIR, "untrusted.key")
    untrusted_csr = os.path.join(CERT_DIR, "untrusted.csr")
    untrusted_crt = os.path.join(CERT_DIR, "untrusted.crt")

    # 1. Root CA
    if not (os.path.exists(ca_crt) and os.path.exists(ca_key)):
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", ca_key, "-out", ca_crt, "-days", "365",
            "-subj", "/CN=MyOS Testnet Root CA"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 2. Server Certificate (Valid)
    if not (os.path.exists(server_crt) and os.path.exists(server_key)):
        with open(san_cnf, "w") as f:
            f.write("[req]\n"
                    "distinguished_name = req_distinguished_name\n"
                    "req_extensions = v3_req\n"
                    "prompt = no\n"
                    "[req_distinguished_name]\n"
                    "CN = testnet.local\n"
                    "[v3_req]\n"
                    "subjectAltName = @alt_names\n"
                    "[alt_names]\n"
                    "IP.1 = 10.0.2.2\n"
                    "IP.2 = 127.0.0.1\n"
                    "DNS.1 = localhost\n"
                    "DNS.2 = testnet.local\n"
                    "DNS.3 = api.example.com\n"
                    "DNS.4 = 10.0.2.2\n")

        subprocess.run([
            "openssl", "req", "-newkey", "rsa:2048", "-nodes",
            "-keyout", server_key, "-out", server_csr,
            "-config", san_cnf
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        subprocess.run([
            "openssl", "x509", "-req", "-in", server_csr,
            "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
            "-out", server_crt, "-days", "365",
            "-extfile", san_cnf, "-extensions", "v3_req"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 3. Wrong-host Certificate (CN=wrong.example.com, no SAN for testnet.local)
    if not (os.path.exists(wrong_crt) and os.path.exists(wrong_key)):
        with open(wrong_cnf, "w") as f:
            f.write("[req]\n"
                    "distinguished_name = req_distinguished_name\n"
                    "req_extensions = v3_req\n"
                    "prompt = no\n"
                    "[req_distinguished_name]\n"
                    "CN = wrong.example.com\n"
                    "[v3_req]\n"
                    "subjectAltName = @alt_names\n"
                    "[alt_names]\n"
                    "DNS.1 = wrong.example.com\n")

        subprocess.run([
            "openssl", "req", "-newkey", "rsa:2048", "-nodes",
            "-keyout", wrong_key, "-out", wrong_csr,
            "-config", wrong_cnf
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        subprocess.run([
            "openssl", "x509", "-req", "-in", wrong_csr,
            "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
            "-out", wrong_crt, "-days", "365",
            "-extfile", wrong_cnf, "-extensions", "v3_req"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 4. Expired Certificate (Valid in 2020, expired 2021)
    if not os.path.exists(expired_crt):
        subprocess.run([
            "openssl", "x509", "-req", "-in", server_csr,
            "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
            "-out", expired_crt,
            "-not_before", "20200101000000Z", "-not_after", "20210101000000Z",
            "-extfile", san_cnf, "-extensions", "v3_req"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 5. Untrusted Certificate (Signed by rogue_ca)
    if not (os.path.exists(untrusted_crt) and os.path.exists(untrusted_key)):
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", rogue_key, "-out", rogue_crt, "-days", "365",
            "-subj", "/CN=Rogue Untrusted CA"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        subprocess.run([
            "openssl", "req", "-newkey", "rsa:2048", "-nodes",
            "-keyout", untrusted_key, "-out", untrusted_csr,
            "-config", san_cnf
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        subprocess.run([
            "openssl", "x509", "-req", "-in", untrusted_csr,
            "-CA", rogue_crt, "-CAkey", rogue_key, "-CAcreateserial",
            "-out", untrusted_crt, "-days", "365",
            "-extfile", san_cnf, "-extensions", "v3_req"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    return ca_crt, server_crt, server_key, wrong_crt, wrong_key, expired_crt, untrusted_crt, untrusted_key


def run_http_server(host, port):
    httpd = HTTPServer((host, port), TestnetHandler)
    print(f"[testnet] HTTP server listening on {host}:{port}")
    httpd.serve_forever()


def run_https_server(host, port, server_crt, server_key, desc="HTTPS"):
    httpd = HTTPServer((host, port), TestnetHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=server_crt, keyfile=server_key)
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    print(f"[testnet] {desc} server listening on {host}:{port}")
    httpd.serve_forever()


def run_tcp_echo(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(32)
    print(f"[testnet] TCP echo listening on {host}:{port}")
    while True:
        try:
            conn, _ = s.accept()
            threading.Thread(target=handle_tcp_conn, args=(conn,), daemon=True).start()
        except Exception:
            break


def handle_tcp_conn(conn):
    with conn:
        while True:
            data = conn.recv(16384)
            if not data:
                break
            conn.sendall(data)


def run_udp_echo(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((host, port))
    print(f"[testnet] UDP echo listening on {host}:{port}")
    while True:
        try:
            data, addr = s.recvfrom(4096)
            if data:
                s.sendto(data, addr)
        except Exception:
            break


def run_dns_stub(host, port):
    global s_rebind_query_count
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((host, port))
    print(f"[testnet] DNS stub listening on {host}:{port}")
    while True:
        try:
            data, addr = s.recvfrom(512)
            if len(data) < 12:
                continue

            query_id = data[:2]
            # Parse question
            idx = 12
            labels = []
            while idx < len(data) and data[idx] != 0:
                llen = data[idx]
                idx += 1
                labels.append(data[idx:idx+llen].decode("latin1", errors="replace"))
                idx += llen
            idx += 1  # null root
            qname = ".".join(labels).lower()
            qtype = struct.unpack(">H", data[idx:idx+2])[0] if idx + 2 <= len(data) else 1
            question_data = data[12:idx+4]

            # Rebinding logic:
            # If rebind.testnet.local:
            # 1st query -> 10.0.2.2
            # 2nd query -> 127.0.0.1
            ip_str = "10.0.2.2"
            if "rebind" in qname:
                with s_rebind_lock:
                    s_rebind_query_count += 1
                    count = s_rebind_query_count
                if count > 1:
                    ip_str = "127.0.0.1"
                else:
                    ip_str = "10.0.2.2"

            ip_bytes = socket.inet_aton(ip_str)

            # Build DNS response
            flags = struct.pack(">H", 0x8180)  # Standard response, no error
            counts = struct.pack(">HHHH", 1, 1, 0, 0)  # 1 question, 1 answer
            hdr = query_id + flags + counts

            # Answer section: name ptr (0xc00c), type A (1), class IN (1), ttl (0s), rlength (4), rdata
            ans = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 0, 4) + ip_bytes

            resp = hdr + question_data + ans
            s.sendto(resp, addr)
        except Exception as e:
            sys.stderr.write(f"[testnet-dns] error: {e}\n")


def main():
    if "--cert-only" in sys.argv:
        ca_crt, *_ = ensure_test_certificates()
        print(f"[testnet] Root CA cert generated at: {ca_crt}")
        return

    host = "0.0.0.0"
    (ca_crt, server_crt, server_key,
     wrong_crt, wrong_key,
     expired_crt,
     untrusted_crt, untrusted_key) = ensure_test_certificates()

    print(f"[testnet] Root CA cert generated at: {ca_crt}")

    # Start HTTP on 8080
    threading.Thread(target=run_http_server, args=(host, HTTP_PORT), daemon=True).start()

    # Start HTTPS valid on 8443
    threading.Thread(target=run_https_server, args=(host, HTTPS_PORT, server_crt, server_key, "HTTPS Valid"), daemon=True).start()

    # Start HTTPS wrong-host on 8444
    threading.Thread(target=run_https_server, args=(host, HTTPS_WRONG_PORT, wrong_crt, wrong_key, "HTTPS Wrong-Host"), daemon=True).start()

    # Start HTTPS expired on 8445
    threading.Thread(target=run_https_server, args=(host, HTTPS_EXPIRED_PORT, expired_crt, server_key, "HTTPS Expired"), daemon=True).start()

    # Start HTTPS untrusted CA on 8446
    threading.Thread(target=run_https_server, args=(host, HTTPS_UNTRUSTED_PORT, untrusted_crt, untrusted_key, "HTTPS Untrusted CA"), daemon=True).start()

    # Start TCP Echo on 7777
    threading.Thread(target=run_tcp_echo, args=(host, TCP_ECHO_PORT), daemon=True).start()

    # Start UDP Echo on 7777
    threading.Thread(target=run_udp_echo, args=(host, UDP_ECHO_PORT), daemon=True).start()

    # Start DNS Stub on 1053
    threading.Thread(target=run_dns_stub, args=(host, DNS_PORT), daemon=True).start()

    print("[testnet] Ready for connections on all ports (HTTP 8080, HTTPS 8443..8446, TCP/UDP 7777, DNS 1053).")
    sys.stdout.flush()

    while True:
        try:
            time.sleep(1)
        except KeyboardInterrupt:
            print("[testnet] Shutting down.")
            break


if __name__ == "__main__":
    main()
