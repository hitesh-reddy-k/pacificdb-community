import json
import socket
import ssl
from typing import Any


class PacificDBClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9000,
                 user_id: str = "system", database: str = "",
                 use_tls: bool = False, ca_file: str | None = None,
                 timeout: float = 30.0):
        self.host, self.port = host, port
        self.user_id, self.database = user_id, database
        self.use_tls, self.ca_file, self.timeout = use_tls, ca_file, timeout
        self.token = ""

    def request(self, command: dict[str, Any]) -> Any:
        payload = {"userId": self.user_id, "dbName": self.database}
        if self.token:
            payload["token"] = self.token
        payload.update(command)
        connection = socket.create_connection((self.host, self.port), self.timeout)
        try:
            if self.use_tls:
                context = ssl.create_default_context(cafile=self.ca_file)
                connection = context.wrap_socket(connection, server_hostname=self.host)
            connection.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
            response = bytearray()
            while b"\n" not in response:
                chunk = connection.recv(65536)
                if not chunk:
                    raise ConnectionError("PacificDB closed before returning JSON")
                response.extend(chunk)
            value = json.loads(response.split(b"\n", 1)[0])
            if isinstance(value, dict) and value.get("error"):
                raise RuntimeError(str(value["error"]))
            return value
        finally:
            connection.close()

    def authenticate(self, username: str, password: str) -> dict[str, Any]:
        result = self.request({
            "action": "security_authenticate", "username": username, "password": password
        })
        self.token = result["token"]
        return result

    def create_database(self, name: str | None = None, db_type: str = "binary") -> Any:
        return self.request({
            "action": "createDatabase", "dbName": name or self.database, "dbType": db_type
        })

    def create_collection(self, name: str) -> Any:
        return self.request({"action": "createCollection", "collection": name})

    def insert(self, collection: str, document: dict[str, Any]) -> Any:
        return self.request({"action": "insert", "collection": collection, "data": document})

    def find(self, collection: str, filter: dict[str, Any] | None = None,
             limit: int = -1, offset: int = 0) -> Any:
        return self.request({
            "action": "find", "collection": collection, "filter": filter or {},
            "limit": limit, "offset": offset
        })

    def update_one(self, collection: str, filter: dict[str, Any],
                   update: dict[str, Any]) -> Any:
        return self.request({
            "action": "updateOne", "collection": collection,
            "filter": filter, "update": update
        })

    def delete_one(self, collection: str, filter: dict[str, Any]) -> Any:
        return self.request({
            "action": "deleteOne", "collection": collection, "filter": filter
        })
