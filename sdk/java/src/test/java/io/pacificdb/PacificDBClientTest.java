package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.Map;
import java.util.concurrent.Executors;

import static org.junit.jupiter.api.Assertions.assertEquals;

class PacificDBClientTest {
    @Test
    void sendsAndReceivesOneJsonLine() throws Exception {
        ObjectMapper json = new ObjectMapper();
        try (ServerSocket server = new ServerSocket(0)) {
            var executor = Executors.newSingleThreadExecutor();
            var future = executor.submit(() -> {
                try (Socket socket = server.accept()) {
                    Map<?, ?> request = json.readValue(
                        new BufferedReader(new InputStreamReader(socket.getInputStream())).readLine(),
                        Map.class);
                    new PrintWriter(socket.getOutputStream(), true).println(
                        json.writeValueAsString(Map.of("ok", true, "action", request.get("action"))));
                }
                return null;
            });
            PacificDBClient client = new PacificDBClient("127.0.0.1", server.getLocalPort(), "app");
            assertEquals("ping", client.request(Map.of("action", "ping")).get("action"));
            future.get();
            executor.shutdown();
        }
    }
}
