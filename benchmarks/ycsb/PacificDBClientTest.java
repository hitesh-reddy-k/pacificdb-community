package site.ycsb.db;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import site.ycsb.*;

/** Exercise the real binding over sockets; no engine or external framework required. */
public class PacificDBClientTest {
  static class Endpoint implements AutoCloseable {
    final ServerSocket server = new ServerSocket(0, 16, InetAddress.getLoopbackAddress());
    final ExecutorService workers = Executors.newCachedThreadPool();
    final List<Socket> sockets = Collections.synchronizedList(new ArrayList<>());
    final AtomicInteger writes = new AtomicInteger();
    final AtomicInteger notReadyResponses = new AtomicInteger();
    volatile boolean leader = true;
    volatile boolean dropWrite;
    volatile String writeResponse = "{\"status\":\"updated\"}";
    volatile String readResponse = "{\"data\":[{\"id\":\"user1\",\"field0\":\"value\"}]}";
    Endpoint() throws IOException {
      workers.submit(() -> {
        try {
          while (!server.isClosed()) {
            Socket socket = server.accept();
            sockets.add(socket);
            workers.submit(() -> serve(socket));
          }
        } catch (IOException expectedOnClose) { }
      });
    }
    String address() { return "127.0.0.1:" + server.getLocalPort(); }
    void serve(Socket socket) {
      try (socket; BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8))) {
        for (String line; (line = in.readLine()) != null;) {
          String response;
          if (line.contains("admin_raft_status")) {
            boolean ready = notReadyResponses.getAndUpdate(value -> value > 0 ? value - 1 : 0) == 0;
            response = "{\"ok\":true,\"isLeader\":" + leader
                + ",\"recoveryComplete\":" + ready + "}";
          }
          else if (!leader) response = "{\"error\":\"not_leader\"}";
          else if (line.contains("\"action\":\"find\"")) response = readResponse;
          else {
            writes.incrementAndGet();
            if (dropWrite) return;
            response = writeResponse;
          }
          socket.getOutputStream().write((response + "\n").getBytes(StandardCharsets.UTF_8));
        }
      } catch (IOException expectedOnClose) { }
    }
    public void close() throws IOException {
      server.close();
      synchronized (sockets) { for (Socket socket : sockets) socket.close(); }
      workers.shutdownNow();
    }
  }
  static void check(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }
  public static void main(String[] args) throws Exception {
    try (Endpoint first = new Endpoint(); Endpoint second = new Endpoint()) {
      second.leader = false;
      PacificDBClient client = new PacificDBClient();
      Properties properties = new Properties();
      properties.setProperty("pacificdb.url", first.address());
      properties.setProperty("pacificdb.urls", first.address() + "," + second.address());
      properties.setProperty("pacificdb.tls", "false");
      properties.setProperty("pacificdb.authRequired", "false");
      properties.setProperty("pacificdb.binaryProtocol", "false");
      properties.setProperty("pacificdb.initializeSchema", "false");
      properties.setProperty("pacificdb.timeoutMs", "1000");
      properties.setProperty("fieldcount", "1");
      client.setProperties(properties);
      client.init();
      try {
        HashMap<String, ByteIterator> result = new HashMap<>();
        check(client.read("t", "user1", null, result).equals(Status.OK), "read must succeed");
        check(result.containsKey("field0") && result.get("field0").toString().equals("value"),
            "read must decode fields into YCSB's result");
        first.readResponse = "{\"data\":[]}";
        check(client.read("t", "user1", null, result).equals(Status.NOT_FOUND), "missing document passed");
        first.readResponse = "{}";
        check(client.read("t", "user1", null, result).equals(Status.ERROR), "malformed success passed");
        first.readResponse = "{\"data\":[{\"id\":\"user1\"}]}";
        check(client.read("t", "user1", null, result).equals(Status.ERROR), "missing fields passed");
        first.readResponse = "{\"data\":[{\"id\":\"wrong\",\"field0\":\"value\"}]}";
        check(client.read("t", "user1", null, result).equals(Status.ERROR), "wrong document passed");
        first.leader = false;
        second.leader = true;
        check(client.read("t", "user1", null, result).equals(Status.OK), "read did not find new leader");
        Map<String, ByteIterator> values = new HashMap<>();
        values.put("field0", new StringByteIterator("updated"));
        first.leader = true;
        second.leader = false;
        check(client.update("t", "user1", values).equals(Status.OK), "write did not find new leader");
        first.readResponse = "{\"error\":\"term_changed_during_read\",\"isLeader\":false}";
        // The old endpoint may report leadership loss in a response other than not_leader.
        first.notReadyResponses.set(1);
        second.leader = true;
        check(client.read("t", "user1", null, result).equals(Status.OK),
            "term-changed read did not rediscover leader");
        second.writeResponse = "{\"error\":\"write_not_committed\",\"isLeader\":false}";
        int ambiguousBefore = second.writes.get();
        check(client.update("t", "user1", values).equals(Status.ERROR), "uncommitted write passed");
        check(second.writes.get() == ambiguousBefore + 1, "uncommitted write was replayed");
        second.leader = false;
        first.readResponse = "{\"data\":[{\"id\":\"user1\",\"field0\":\"value\"}]}";
        check(client.read("t", "user1", null, result).equals(Status.OK),
            "next operation reused stale leader after ambiguous write");
        int before = first.writes.get();
        first.dropWrite = true;
        check(client.update("t", "user1", values).equals(Status.ERROR), "ambiguous write passed");
        check(first.writes.get() == before + 1, "ambiguous mutation replayed without deduplication");
        first.leader = false;
        second.leader = true;
        second.notReadyResponses.set(1);
        PacificDBClient retryingClient = new PacificDBClient();
        retryingClient.setProperties(properties);
        retryingClient.init();
        retryingClient.cleanup();
        System.out.println("PACIFICDB_BINDING_PASS");
      } finally { client.cleanup(); }
    }
  }
}
