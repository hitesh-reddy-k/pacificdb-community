import json
import socketserver
import threading

from pacificdb import PacificDBClient


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        request = json.loads(self.rfile.readline())
        self.wfile.write(json.dumps({
            "ok": True, "action": request["action"], "database": request["dbName"]
        }).encode() + b"\n")


def test_request_round_trip():
    with socketserver.TCPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.handle_request)
        thread.start()
        client = PacificDBClient("127.0.0.1", server.server_address[1], database="app")
        assert client.request({"action": "ping"}) == {
            "ok": True, "action": "ping", "database": "app"
        }
        thread.join()
