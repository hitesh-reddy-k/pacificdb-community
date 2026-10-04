package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;

final class TestPeer implements AutoCloseable {
    private final ServerSocket server = new ServerSocket(0);
    private final ExecutorService workers = Executors.newCachedThreadPool(r -> { Thread t = new Thread(r); t.setDaemon(true); return t; });
    private final Set<Socket> sockets = ConcurrentHashMap.newKeySet();
    final List<Map<String,Object>> frames = new CopyOnWriteArrayList<>();
    final AtomicInteger connections = new AtomicInteger();
    final int port = server.getLocalPort();
    private static final ObjectMapper JSON = new ObjectMapper();
    TestPeer(Function<Map<String,Object>,Object> reply) throws IOException {
        workers.submit(() -> {
            try {
                while (!server.isClosed()) {
                    Socket socket = server.accept(); sockets.add(socket); connections.incrementAndGet();
                    workers.submit(() -> handle(socket, reply));
                }
            } catch (IOException ignored) { }
        });
    }
    @SuppressWarnings("unchecked")
    private void handle(Socket socket, Function<Map<String,Object>,Object> reply) {
        try (Socket owned = socket; var reader = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null;) {
                Map<String,Object> request = JSON.readValue(line, Map.class); frames.add(request);
                Object response = reply.apply(request);
                if (response == null) return;
                if (response instanceof byte[][]) {
                    for (byte[] chunk : (byte[][])response) { socket.getOutputStream().write(chunk); socket.getOutputStream().flush(); Thread.sleep(1); }
                } else {
                    byte[] bytes = response instanceof byte[] ? (byte[])response : (JSON.writeValueAsString(response)+"\n").getBytes(StandardCharsets.UTF_8);
                    socket.getOutputStream().write(bytes); socket.getOutputStream().flush();
                }
                if (!(response instanceof Map) || !Boolean.TRUE.equals(((Map<?,?>)response).get("_pacificdb_connection_keepalive")) || Boolean.TRUE.equals(((Map<?,?>)response).get("_pacificdb_connection_close"))) return;
            }
        } catch (IOException | InterruptedException ignored) { }
        finally { sockets.remove(socket); }
    }
    public void close() throws Exception {
        server.close();
        for (Socket socket : sockets) socket.close();
        workers.shutdownNow(); assert workers.awaitTermination(3, TimeUnit.SECONDS);
    }
}
