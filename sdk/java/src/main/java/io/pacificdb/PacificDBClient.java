package io.pacificdb;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;

import javax.net.SocketFactory;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

public final class PacificDBClient {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final String host;
    private final int port;
    private final String userId;
    private final String database;
    private final boolean tls;
    private final int timeoutMs;
    private final SocketFactory socketFactory;
    private String token = "";

    public PacificDBClient(String host, int port, String userId,
                           String database, boolean tls, int timeoutMs) {
        this(host, port, userId, database, tls, timeoutMs,
            tls ? SSLSocketFactory.getDefault() : SocketFactory.getDefault());
    }

    PacificDBClient(String host, int port, String userId,
                    String database, boolean tls, int timeoutMs,
                    SocketFactory socketFactory) {
        this.host = host;
        this.port = port;
        this.userId = userId;
        this.database = database;
        this.tls = tls;
        this.timeoutMs = timeoutMs;
        this.socketFactory = socketFactory;
    }

    public PacificDBClient(String host, int port, String database) {
        this(host, port, "system", database, false, 30_000);
    }

    public Map<String, Object> request(Map<String, Object> command) throws Exception {
        try (Socket socket = socketFactory.createSocket()) {
            socket.connect(new InetSocketAddress(host, port), timeoutMs);
            socket.setSoTimeout(timeoutMs);
            if (tls) {
                SSLSocket sslSocket = (SSLSocket) socket;
                SSLParameters parameters = sslSocket.getSSLParameters();
                parameters.setEndpointIdentificationAlgorithm("HTTPS");
                sslSocket.setSSLParameters(parameters);
                sslSocket.startHandshake();
            }
            Map<String, Object> payload = new LinkedHashMap<>();
            payload.put("userId", userId);
            payload.put("dbName", database);
            if (!token.isEmpty()) payload.put("token", token);
            payload.putAll(command);
            BufferedWriter writer = new BufferedWriter(
                new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8));
            writer.write(JSON.writeValueAsString(payload));
            writer.newLine();
            writer.flush();
            BufferedReader reader = new BufferedReader(
                new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
            String line = reader.readLine();
            if (line == null) throw new IllegalStateException("PacificDB closed before returning JSON");
            Map<String, Object> response = JSON.readValue(
                line, new TypeReference<Map<String, Object>>() {});
            if (response.containsKey("error"))
                throw new IllegalStateException(String.valueOf(response.get("error")));
            return response;
        }
    }

    public Map<String, Object> authenticate(String username, String password) throws Exception {
        Map<String, Object> command = new LinkedHashMap<>();
        command.put("action", "security_authenticate");
        command.put("username", username);
        command.put("password", password);
        Map<String, Object> response = request(command);
        token = String.valueOf(response.get("token"));
        return response;
    }
}
