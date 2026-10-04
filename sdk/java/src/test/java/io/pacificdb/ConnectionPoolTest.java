package io.pacificdb;

import org.junit.jupiter.api.Test;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.jupiter.api.Assertions.*;

class ConnectionPoolTest {
    private String url(TestPeer peer) { return "pacificdb://127.0.0.1:"+peer.port+"/app"; }
    private static void await(CountDownLatch latch) {
        try { assertTrue(latch.await(2, TimeUnit.SECONDS)); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }
    @Test void arraysUnicodeAndReuseWithoutRetries() throws Exception {
        try (var server = new TestPeer(req -> List.of("app", "café")); var db = PacificDB.fromUrl(url(server))) {
            assertEquals(List.of("app", "café"), db.requestValue(Map.of("action", "listDatabases")));
            assertEquals("unexpected_response_shape", assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "listDatabases"))).getCode());
        }
        try (var server = new TestPeer(req -> new byte[][] { "{\"word\":\"caf".getBytes(StandardCharsets.UTF_8), new byte[] {(byte)0xc3}, new byte[] {(byte)0xa9, '"', '}', '\n'} }); var db = PacificDB.fromUrl(url(server))) {
            assertEquals("café", db.request(Map.of("action", "find")).get("word"));
        }
        try (var server = new TestPeer(req -> Map.of("ok", true, "_pacificdb_connection_keepalive", true)); var db = PacificDB.fromUrl(url(server))) {
            for (int i=0;i<3;i++) assertEquals(Map.of("ok", true), db.request(Map.of("action", "ping")));
            assertEquals(1, server.connections.get());
        }
        try (var server = new TestPeer(req -> Map.of("ok", true)); var db = PacificDB.fromUrl(url(server))) {
            for (int i=0;i<3;i++) db.request(Map.of("action", "ping"));
            assertEquals(3, server.connections.get());
        }
    }
    @Test void invalidFramesAndLostReplyNeverReplayWrite() throws Exception {
        for (byte[] bytes : new byte[][] { "invalid\n".getBytes(), "{\"x\":NaN}\n".getBytes(), "{}\n{}\n".getBytes(), "{} junk\n".getBytes(), new byte[] {'"',(byte)0xff,'"','\n'}, null, ("\""+"x".repeat(200)+"\"\n").getBytes() }) {
            try (var server = new TestPeer(req -> bytes); var db = PacificDB.fromUrl(url(server), Map.of("maxResponseBytes", 128))) {
                var error = assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert")));
                assertTrue(Set.of("invalid_response", "connection_lost", "response_too_large").contains(error.getCode()));
                assertNull(error.getCause()); assertEquals(1, server.frames.size());
            }
        }
        try (var server = new TestPeer(req -> "{\"ok\":true,\"_pacificdb_connection_keepalive\":true}\n".getBytes(StandardCharsets.UTF_8));
             var db = PacificDB.fromUrl(url(server))) {
            assertEquals(Map.of("ok", true), db.request(Map.of("action", "ping")));
            assertEquals("connection_lost", assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert"))).getCode());
            assertEquals(1, server.frames.size()); assertEquals(1, server.connections.get());
        }
    }
    @Test void authOnceAndPrivateErrors() throws Exception {
        try (var server = new TestPeer(req -> req.get("action").equals("security_authenticate") ? Map.of("token", "private-token", "_pacificdb_connection_keepalive", true) : Map.of("ok", req.get("token").equals("private-token"), "_pacificdb_connection_keepalive", true)); var db = PacificDB.fromUrl(url(server).replace("//", "//demo:private-password@"))) {
            var workers = Executors.newFixedThreadPool(8);
            try {
                var futures = new ArrayList<Future<Map<String,Object>>>();
                for (int i=0;i<8;i++) futures.add(workers.submit(() -> db.request(Map.of("action", "find"))));
                for (var future : futures) assertEquals(Map.of("ok", true), future.get(2, TimeUnit.SECONDS));
            } finally { workers.shutdownNow(); }
            assertEquals(1, server.frames.stream().filter(frame -> frame.get("action").equals("security_authenticate")).count());
            assertFalse(db.toString().contains("private-password")); assertFalse(db.toString().contains("private-token"));
        }
        try (var server = new TestPeer(req -> Map.of("error", "denied", "message", req.get("password"), "token", "private-token"))) {
            var error = assertThrows(PacificDBException.class, () -> PacificDB.connect(url(server).replace("//", "//demo:private-password@")));
            assertEquals("denied", error.getCode()); assertNull(error.getCause());
            assertFalse(error.toString().contains("private-password")); assertFalse(error.getResponse().toString().contains("private-token"));
            assertEquals(1, server.frames.size());
        }
        try (var server = new TestPeer(req -> Map.of("error", Map.of("code", "denied", "message", req.get("token")))); var db = PacificDB.fromUrl(url(server))) {
            var error = assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert", "token", "raw-secret-token")));
            assertEquals("denied", error.getCode()); assertFalse(error.toString().contains("raw-secret-token"));
        }
    }
    @Test @SuppressWarnings("unchecked") void queuedInputCaptureAndCloseWakeup() throws Exception {
        var entered = new CountDownLatch(1); var release = new CountDownLatch(1);
        try (var server = new TestPeer(req -> { if (req.get("action").equals("hold")) { entered.countDown(); await(release); } return Map.of("scope", req, "_pacificdb_connection_keepalive", true); }); var db = PacificDB.fromUrl(url(server), Map.of("poolSize", 1));) {
            var workers = Executors.newFixedThreadPool(3);
            try {
                var active = workers.submit(() -> db.request(Map.of("action", "hold"))); await(entered);
                var data = new LinkedHashMap<String,Object>(); data.put("value", "original");
                var command = new LinkedHashMap<String,Object>(); command.put("action", "insert"); command.put("data", data);
                var queuedThread = new AtomicReference<Thread>();
                var queued = workers.submit(() -> { queuedThread.set(Thread.currentThread()); return db.request(command); });
                long until = System.nanoTime()+TimeUnit.SECONDS.toNanos(1);
                while ((queuedThread.get()==null || queuedThread.get().getState()!=Thread.State.TIMED_WAITING) && System.nanoTime()<until) Thread.yield();
                assertEquals(Thread.State.TIMED_WAITING, queuedThread.get().getState());
                data.put("value", "changed"); release.countDown(); active.get(2, TimeUnit.SECONDS);
                var scope = (Map<String,Object>)queued.get(2, TimeUnit.SECONDS).get("scope");
                assertEquals("original", ((Map<?,?>)scope.get("data")).get("value")); assertEquals("app", scope.get("dbName"));
                db.close(); db.close();
                assertEquals("client_closed", assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert"))).getCode());
            } finally { release.countDown(); workers.shutdownNow(); }
        }
    }
    @Test void requestDeadlineBoundsIncompleteReply() throws Exception {
        try (var server = new TestPeer(req -> { try { Thread.sleep(300); } catch (InterruptedException ignored) { } return "{\"x\":".getBytes(); }); var db = PacificDB.fromUrl(url(server), Map.of("timeoutMs", 50))) {
            long started = System.nanoTime();
            assertEquals("request_timeout", assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert"))).getCode());
            assertTrue(System.nanoTime()-started < TimeUnit.MILLISECONDS.toNanos(250));
        }
    }

    @Test void poolLimitAndCloseWakeActiveAndQueuedRequests() throws Exception {
        var entered = new CountDownLatch(2); var release = new CountDownLatch(1);
        try (var server = new TestPeer(req -> { entered.countDown(); await(release); return Map.of("ok", true); });
             var db = PacificDB.fromUrl(url(server), Map.of("poolSize", 2))) {
            var workers = Executors.newFixedThreadPool(8);
            try {
                var started = new CountDownLatch(8); var results = new ArrayList<Future<String>>();
                for (int i = 0; i < 8; i++) results.add(workers.submit(() -> {
                    started.countDown();
                    return assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert"))).getCode();
                }));
                await(started); await(entered);
                assertEquals(2, server.connections.get());
                db.close(); db.close();
                for (var result : results) assertEquals("client_closed", result.get(1, TimeUnit.SECONDS));
                assertEquals(2, server.frames.size());
            } finally { release.countDown(); workers.shutdownNow(); }
        }
    }

    @Test void queueDeadlineDoesNotTerminateAnUnrelatedLongerLease() throws Exception {
        var entered = new CountDownLatch(1); var release = new CountDownLatch(1);
        try (var server = new TestPeer(req -> { entered.countDown(); await(release); return Map.of("ok", true); });
             var pool = new ConnectionPool(ConnectionOptions.fromUrl(url(server)).withOverrides(Map.of("poolSize", 1)), null)) {
            var worker = Executors.newSingleThreadExecutor();
            byte[] wire = "{\"action\":\"insert\"}\n".getBytes(StandardCharsets.UTF_8);
            try {
                var active = worker.submit(() -> pool.request(wire, System.nanoTime() + TimeUnit.SECONDS.toNanos(2))); await(entered);
                assertEquals("request_timeout", assertThrows(PacificDBException.class,
                    () -> pool.request(wire, System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(40))).getCode());
                assertEquals(1, server.frames.size());
                release.countDown(); assertEquals(Map.of("ok", true), active.get(1, TimeUnit.SECONDS));
            } finally { release.countDown(); worker.shutdownNow(); }
        }
    }

    @Test void authFailureWakesWaitersAndMissingTokenClosesLazyClient() throws Exception {
        var entered = new CountDownLatch(1); var release = new CountDownLatch(1);
        try (var server = new TestPeer(req -> { entered.countDown(); await(release); return Map.of("error", "denied", "message", req.get("password")); });
             var db = PacificDB.fromUrl(url(server).replace("//", "//demo:private-password@"))) {
            var workers = Executors.newFixedThreadPool(8);
            try {
                var results = new ArrayList<Future<String>>();
                for (int i = 0; i < 8; i++) results.add(workers.submit(() -> {
                    var error = assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert")));
                    assertFalse(error.toString().contains("private-password")); assertNull(error.getCause()); return error.getCode();
                }));
                await(entered); release.countDown();
                for (var result : results) assertTrue(Set.of("denied", "client_closed").contains(result.get(2, TimeUnit.SECONDS)));
                assertEquals(1, server.frames.size()); assertEquals("security_authenticate", server.frames.get(0).get("action"));
            } finally { release.countDown(); workers.shutdownNow(); }
        }
        try (var server = new TestPeer(req -> Map.of("ok", true));
             var db = PacificDB.fromUrl(url(server).replace("//", "//demo:private-password@"))) {
            assertEquals("invalid_response", assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert"))).getCode());
            assertEquals("client_closed", assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert"))).getCode());
            assertEquals(1, server.frames.size());
        }
    }

    @Test void stalledWritesAreBoundedAndDeadlineThreadsStopOnClose() throws Exception {
        try (var server = new java.net.ServerSocket(0)) {
            var worker = Executors.newSingleThreadExecutor();
            var accepted = new CountDownLatch(1); var release = new CountDownLatch(1);
            var peer = worker.submit(() -> { try (var socket = server.accept()) { socket.setReceiveBufferSize(1024); accepted.countDown(); await(release); } return null; });
            try (var pool = new ConnectionPool(ConnectionOptions.fromUrl("pacificdb://127.0.0.1:" + server.getLocalPort() + "/app"), null)) {
                byte[] wire = ("{\"action\":\"insert\",\"data\":\"" + "x".repeat(16 * 1024 * 1024) + "\"}\n").getBytes(StandardCharsets.UTF_8);
                long start = System.nanoTime();
                var error = assertThrows(PacificDBException.class, () -> pool.request(wire, System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(100)));
                assertEquals("request_timeout", error.getCode()); await(accepted);
                assertTrue(error.getMessage().contains("outcome may be unknown"));
                assertTrue(System.nanoTime() - start < TimeUnit.SECONDS.toNanos(2));
            } finally { release.countDown(); server.close(); peer.get(2, TimeUnit.SECONDS); worker.shutdownNow(); }
        }
        try (var server = new TestPeer(req -> Map.of("ok", true))) {
            for (int i = 0; i < 10; i++) {
                try (var db = PacificDB.fromUrl(url(server))) { db.request(Map.of("action", "ping")); }
            }
        }
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (deadlineThreads() != 0 && System.nanoTime() < until) Thread.yield();
        assertEquals(0, deadlineThreads());
    }
    private long deadlineThreads() {
        return Thread.getAllStackTraces().keySet().stream().filter(thread -> thread.isAlive() && thread.getName().equals("pacificdb-deadlines")).count();
    }
    @Test void invalidCallerJsonDoesNotOpenSocketsOrLeakPrivateValues() throws Exception {
        try (var server = new TestPeer(req -> Map.of("ok", true)); var db = PacificDB.fromUrl(url(server))) {
            var cyclic = new LinkedHashMap<String, Object>(); cyclic.put("cycle", cyclic);
            for (var data : List.of(cyclic, Map.of("value", Double.NaN))) {
                var error = assertThrows(PacificDBException.class, () -> db.request(Map.of("action", "insert", "data", data)));
                assertEquals("invalid_request", error.getCode()); assertNull(error.getCause());
            }
            assertEquals(0, server.connections.get());
        }
    }
}
