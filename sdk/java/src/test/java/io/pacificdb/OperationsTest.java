package io.pacificdb;

import org.junit.jupiter.api.Test;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Function;
import static org.junit.jupiter.api.Assertions.*;

class OperationsTest {
    @Test void directLifecycleAndContextOnlyChangeOnSuccess() throws Exception {
        try (var server = new TestPeer(req -> {
            if (req.get("action").equals("listDatabases")) return List.of("app", "shop");
            if (req.get("dbName").equals("forbidden")) return Map.of("error", "permission_denied");
            return Map.of("status", "ok");
        }); var db = new PacificDBClient("127.0.0.1", server.port, "")) {
            assertEquals("database_required", assertThrows(PacificDBException.class, () -> db.createCollection("items")).getCode());
            assertTrue(server.frames.isEmpty()); assertEquals(Map.of("status", "ok"), db.capabilities());
            db.createDatabase("app"); assertEquals("app", db.getDatabase()); db.createCollection("items");
            assertEquals("permission_denied", assertThrows(PacificDBException.class, () -> db.createDatabase("forbidden")).getCode());
            assertEquals("app", db.getDatabase()); assertEquals(List.of("app", "shop"), db.listDatabases());
            assertEquals("database_not_found", assertThrows(PacificDBException.class, () -> db.useDatabase("missing")).getCode());
            assertEquals("app", db.getDatabase()); db.useDatabase("shop"); assertEquals("shop", db.getDatabase());
            assertThrows(PacificDBException.class, () -> db.dropDatabase("forbidden")); assertEquals("shop", db.getDatabase());
            db.dropDatabase("app"); assertEquals("shop", db.getDatabase()); db.dropDatabase(); assertEquals("", db.getDatabase());
            assertTrue(server.frames.stream().noneMatch(req -> req.get("action").toString().toLowerCase().contains("project")));
        }
    }
    @Test void methodsPreserveWireFieldsAndResponses() throws Exception {
        var row = Map.<String,Object>of("id", "1");
        var patch = Map.<String,Object>of("$set", Map.of("n", 2));
        var ops = List.of(Map.<String,Object>of("action", "insertOne", "data", row));
        var pipeline = List.of(Map.<String,Object>of("$limit", 1));
        List<Function<PacificDBClient,Object>> calls = List.of(
            db -> db.createCollection("items"), db -> db.listCollections(), db -> db.dropCollection("items"),
            db -> db.insert("items", row), db -> db.insertMany("items", List.of(row)), db -> db.find("items"),
            db -> db.find("items", row, 3, 2), db -> db.count("items"), db -> db.aggregate("items", pipeline),
            db -> db.explain("items"), db -> db.updateOne("items", row, patch), db -> db.updateMany("items", Map.of(), patch),
            db -> db.deleteOne("items", row), db -> db.deleteMany("items", Map.of()), db -> db.bulkWrite("items", ops));
        List<Map<String,Object>> expected = List.of(
            Map.of("action","createCollection","collection","items"), Map.of("action","listCollections"), Map.of("action","dropCollection","collection","items"),
            Map.of("action","insert","collection","items","data",row), Map.of("action","insertMany","collection","items","data",List.of(row)),
            Map.of("action","find","collection","items","filter",Map.of(),"limit",-1,"offset",0),
            Map.of("action","find","collection","items","filter",row,"limit",3,"offset",2),
            Map.of("action","count","collection","items","filter",Map.of()), Map.of("action","aggregate","collection","items","pipeline",pipeline),
            Map.of("action","explain","collection","items","filter",Map.of()), Map.of("action","updateOne","collection","items","filter",row,"update",patch),
            Map.of("action","updateMany","collection","items","filter",Map.of(),"update",patch), Map.of("action","deleteOne","collection","items","filter",row),
            Map.of("action","deleteMany","collection","items","filter",Map.of()), Map.of("action","bulkWrite","collection","items","ops",ops));
        try (var server = new TestPeer(req -> Map.of("status","ok","echo",req)); var db = new PacificDBClient("127.0.0.1",server.port,"app")) {
            for (int i=0;i<calls.size();i++) {
                var wire = new LinkedHashMap<>(expected.get(i)); wire.put("userId","system"); wire.put("dbName","app");
                assertEquals(Map.of("status","ok","echo",wire), calls.get(i).apply(db));
                assertEquals(i+1, server.frames.size());
            }
        }
    }
    @Test void findOneAndLocalValidationAndTypedWriteErrors() throws Exception {
        for (Object response : List.of(List.of(Map.of("id","1")), Map.of("data",List.of(Map.of("id","1"))))) {
            try (var server = new TestPeer(req -> response); var db = new PacificDBClient("127.0.0.1",server.port,"app")) {
                assertEquals(Map.of("id","1"), db.findOne("items"));
                assertEquals(1, server.frames.get(0).get("limit"));
            }
        }
        try (var server = new TestPeer(req -> List.of()); var db = new PacificDBClient("127.0.0.1",server.port,"app")) { assertNull(db.findOne("items")); }
        try (var server = new TestPeer(req -> Map.of("error",Map.of("code","conflict","message","retry explicitly"),"detail",17)); var db = new PacificDBClient("127.0.0.1",server.port,"app")) {
            assertThrows(PacificDBException.class, () -> db.find("items",Map.of(),-2,0));
            assertThrows(PacificDBException.class, () -> db.find("items",Map.of(),1,-1));
            assertThrows(PacificDBException.class, () -> db.createCollection("")); assertTrue(server.frames.isEmpty());
            for (var call : List.<Function<PacificDBClient,Object>>of(client -> client.updateMany("items",Map.of(),Map.of()), client -> client.deleteMany("items",Map.of()), client -> client.bulkWrite("items",List.of()))) {
                var failure = assertThrows(PacificDBException.class, () -> call.apply(db));
                assertEquals("conflict",failure.getCode()); assertEquals(17, ((Map<?,?>)failure.getResponse()).get("detail"));
                assertEquals("app",db.getDatabase());
            }
        }
    }
    @Test void dropCompletionDoesNotClearANewerVerifiedSelection() throws Exception {
        var entered = new CountDownLatch(1); var release = new CountDownLatch(1);
        try (var server = new TestPeer(req -> {
            if (req.get("action").equals("listDatabases")) return List.of("new");
            entered.countDown();
            try { assertTrue(release.await(2, TimeUnit.SECONDS)); } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            return Map.of("status", "ok");
        }); var db = new PacificDBClient("127.0.0.1", server.port, "app")) {
            var worker = Executors.newSingleThreadExecutor();
            try {
                var drop = worker.submit(() -> db.dropDatabase()); assertTrue(entered.await(1, TimeUnit.SECONDS));
                db.useDatabase("new"); release.countDown(); drop.get(1, TimeUnit.SECONDS);
                assertEquals("new", db.getDatabase()); assertEquals("app", server.frames.get(0).get("dbName"));
            } finally { release.countDown(); worker.shutdownNow(); }
        }
    }
    @Test void malformedListsAndRowsAreRejectedWithoutChangingContext() throws Exception {
        for (Object response : List.of(Map.of("status", "ok"), List.of(17), Map.of("data", List.of(17)))) {
            try (var server = new TestPeer(req -> response); var db = new PacificDBClient("127.0.0.1", server.port, "app")) {
                assertEquals("unexpected_response_shape", assertThrows(PacificDBException.class, () -> db.useDatabase("new")).getCode());
                assertEquals("unexpected_response_shape", assertThrows(PacificDBException.class, () -> db.findOne("items")).getCode());
                assertEquals("app", db.getDatabase());
            }
        }
    }
}
