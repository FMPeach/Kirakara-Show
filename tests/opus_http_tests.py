"""Exercise Opus HTTP callbacks using a loopback server, with no external service."""
import http.server
import pathlib
import subprocess
import sys
import threading
import urllib.parse

executable, directory = sys.argv[1:]
root = pathlib.Path(directory)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == '/redirect.opus':
            self.send_response(302)
            self.send_header('Location', '/stereo.opus')
            self.end_headers()
            return
        if path in ('/stereo.opus', '/extensionless'):
            file = root / 'stereo.opus'
            mime = 'audio/ogg'
        elif path == '/original.wav':
            file = root / 'original.wav'
            mime = 'audio/wav'
        else:
            self.send_error(404)
            return
        data = file.read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            # Short writes exercise reads that do not align to an Ogg page.
            for offset in range(0, len(data), 137):
                self.wfile.write(data[offset:offset + 137])
        except (BrokenPipeError, ConnectionResetError):
            pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    completed = subprocess.run([executable, str(root), f'http://127.0.0.1:{server.server_port}'], timeout=75)
finally:
    server.shutdown()
    server.server_close()
    thread.join()
raise SystemExit(completed.returncode)
