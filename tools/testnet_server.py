#!/usr/bin/env python3
"""
tools/testnet_server.py - Host mock network server for Phase 6 hermetic tests.
Runs both HTTP and HTTPS services on localhost / QEMU host alias (10.0.2.2).
Provides endpoints:
  - /echo: echoes request method, headers, and body as JSON.
  - /secret: verifies injected secret header (e.g. Authorization: Bearer <token>).
  - /big: returns a large payload (512 KB) to test streaming and truncation.
  - /redirect: HTTP 302 redirect to /echo.
  - /rebinding: simulated DNS rebinding test endpoint.
  - /health: simple health check.
"""

import sys
import os
import json
import ssl
import subprocess
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

HTTP_PORT = int(os.environ.get("TESTNET_HTTP_PORT", "8080"))
HTTPS_PORT = int(os.environ.get("TESTNET_HTTPS_PORT", "8443"))
CERT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "ssl")

EXPECTED_SECRET = "Bearer topsecret-test-token-12345"

class TestnetHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, format, *args):
        # Keep test logs concise
        sys.stderr.write(f"[testnet] {self.command} {self.path} -> {args[0]}\n")

    def _send_json(self, status_code, data):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
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

        # Route matching
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

        elif path_clean == "/big":
            # Generate 512 KB of data
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

        elif path_clean == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/echo")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
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

    if os.path.exists(ca_crt) and os.path.exists(server_crt) and os.path.exists(server_key):
        return ca_crt, server_crt, server_key

    # Generate Root CA
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", ca_key, "-out", ca_crt, "-days", "365",
        "-subj", "/CN=MyOS Testnet Root CA"
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # SAN configuration
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

    # Generate Server Key & CSR
    subprocess.run([
        "openssl", "req", "-newkey", "rsa:2048", "-nodes",
        "-keyout", server_key, "-out", server_csr,
        "-config", san_cnf
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Sign Server Cert with Root CA
    subprocess.run([
        "openssl", "x509", "-req", "-in", server_csr,
        "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
        "-out", server_crt, "-days", "365",
        "-extfile", san_cnf, "-extensions", "v3_req"
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    return ca_crt, server_crt, server_key


def run_http_server(host, port):
    httpd = HTTPServer((host, port), TestnetHandler)
    print(f"[testnet] HTTP server listening on {host}:{port}")
    httpd.serve_forever()


def run_https_server(host, port, server_crt, server_key):
    httpd = HTTPServer((host, port), TestnetHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=server_crt, keyfile=server_key)
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    print(f"[testnet] HTTPS server listening on {host}:{port}")
    httpd.serve_forever()


def main():
    if "--cert-only" in sys.argv:
        ca_crt, _, _ = ensure_test_certificates()
        print(f"[testnet] Root CA cert generated at: {ca_crt}")
        return

    host = "0.0.0.0"
    ca_crt, server_crt, server_key = ensure_test_certificates()
    print(f"[testnet] Root CA cert generated at: {ca_crt}")

    # Start HTTP in thread
    t_http = threading.Thread(target=run_http_server, args=(host, HTTP_PORT), daemon=True)
    t_http.start()

    # Start HTTPS in thread
    t_https = threading.Thread(target=run_https_server, args=(host, HTTPS_PORT, server_crt, server_key), daemon=True)
    t_https.start()

    print("[testnet] Ready for connections.")
    sys.stdout.flush()

    try:
        t_http.join()
        t_https.join()
    except KeyboardInterrupt:
        print("[testnet] Shutting down.")


if __name__ == "__main__":
    main()
