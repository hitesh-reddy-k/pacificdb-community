package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;
import java.nio.file.Path;
import java.util.*;
import static org.junit.jupiter.api.Assertions.*;

class CapabilitiesTest {
    @SuppressWarnings("unchecked") private List<Map<String,Object>> rows() throws Exception {
        var matrix = new ObjectMapper().readValue(Path.of("../contracts/community-capabilities.json").toFile(),Map.class);
        return (List<Map<String,Object>>)matrix.get("actions");
    }
    @SuppressWarnings("unchecked") private Object invoke(PacificDBClient db, Map<String,Object> row) throws Exception {
        Map<String,Object> fields=(Map<String,Object>)row.get("sample"); String[] parts=row.get("java").toString().split("\\.");
        if (parts.length==2) {
            Object family=db.getClass().getMethod(parts[0]).invoke(db);
            try { return family.getClass().getMethod(parts[1],Map.class).invoke(family,fields); }
            catch (java.lang.reflect.InvocationTargetException error) {
                if (error.getCause() instanceof RuntimeException) throw (RuntimeException)error.getCause();
                throw error;
            }
        }
        String method=parts[0]; String collection=(String)fields.get("collection");
        switch(method) {
            case "capabilities": return db.capabilities(); case "listDatabases": return db.listDatabases(); case "listCollections": return db.listCollections();
            case "createDatabase": return db.createDatabase((String)fields.get("dbName")); case "dropDatabase": return db.dropDatabase((String)fields.get("dbName"));
            case "createCollection": return db.createCollection(collection); case "dropCollection": return db.dropCollection(collection);
            case "insert": return db.insert(collection,(Map<String,Object>)fields.get("data"));
            case "insertMany": return db.insertMany(collection,(List<Map<String,Object>>)fields.get("data"));
            case "find": return db.find(collection); case "count": return db.count(collection); case "explain": return db.explain(collection);
            case "aggregate": return db.aggregate(collection,(List<Map<String,Object>>)fields.get("pipeline"));
            case "updateOne": return db.updateOne(collection,(Map<String,Object>)fields.get("filter"),(Map<String,Object>)fields.get("update"));
            case "updateMany": return db.updateMany(collection,(Map<String,Object>)fields.get("filter"),(Map<String,Object>)fields.get("update"));
            case "deleteOne": return db.deleteOne(collection,(Map<String,Object>)fields.get("filter"));
            case "deleteMany": return db.deleteMany(collection,(Map<String,Object>)fields.get("filter"));
            case "bulkWrite": return db.bulkWrite(collection,(List<Map<String,Object>>)fields.get("ops"));
            default: throw new AssertionError("Unbound beginner method");
        }
    }
    @Test @SuppressWarnings("unchecked") void everyPublicCapabilityAndAliasHasExactWireBinding() throws Exception {
        try (var server=new TestPeer(req -> req.get("action").equals("listDatabases") ? List.of("app") : Map.of("status","ok","token","fixture-token","echo",req,"data",List.of(Map.of("id","1"))))) {
            for (var row: rows()) {
                if (row.get("classification").equals("internal")) continue;
                try (var db=new PacificDBClient("127.0.0.1",server.port,"app")) {
                    db.security().useToken("fixture-token"); int before=server.frames.size(); Object result=invoke(db,row);
                    assertEquals(before+1,server.frames.size()); var wire=server.frames.get(before);
                    assertEquals(row.getOrDefault("alias_of",row.get("action")),wire.get("action"));
                    assertEquals(wire.get("action").equals("listDatabases") ? List.of("app") : Map.of("status","ok","token","fixture-token","echo",wire,"data",List.of(Map.of("id","1"))), result);
                    assertEquals("system",wire.get("userId")); assertEquals("app",wire.get("dbName")); assertEquals("fixture-token",wire.get("token"));
                    ((Map<String,Object>)row.get("sample")).forEach((key,value) -> assertEquals(value,wire.get(key)));
                }
            }
        }
    }
    @Test void familyOptionsCannotReplaceScopeAndSecurityLifecycleRedactsAllPasswords() throws Exception {
        try (var server=new TestPeer(req -> {
            if (req.get("action").equals("security_authenticate")) return Map.of("token","first-token");
            if (req.get("action").equals("security_refresh_token")) return Map.of("token","refreshed-token");
            if (req.get("action").equals("security_whoami")) return Map.of("seen",req.get("token"));
            String secret=(String)req.get("newPassword"); return Map.of("error",secret,"message",secret,secret,true);
        }); var db=new PacificDBClient("127.0.0.1",server.port,"app")) {
            for (String key: List.of("action","userId","dbName","db","token")) {
                var options=new LinkedHashMap<String,Object>(); options.put("collection","items"); options.put("fields",Map.of("name",1)); options.put(key,"other");
                assertEquals("invalid_request",assertThrows(PacificDBException.class,() -> db.indexes().create(options)).getCode());
            }
            assertTrue(server.frames.isEmpty());
            db.security().authenticate(Map.of("username","demo","password","private-password"));
            assertEquals(Map.of("seen","first-token"),db.security().whoami()); db.security().refreshToken();
            assertEquals(Map.of("seen","refreshed-token"),db.security().whoami()); db.security().useToken("api-key-token");
            assertEquals(Map.of("seen","api-key-token"),db.security().whoami());
            var error=assertThrows(PacificDBException.class,() -> db.admin().updateTenantUserPassword(Map.of("tenantId","tenant","username","demo","newPassword","private-new-password","updatedBy","admin")));
            assertFalse((error.toString()+error.getCode()+error.getResponse()).contains("private-new-password")); assertNull(error.getCause());
        }
    }
    @Test void everyNamedMethodPreservesEngineErrorAndSelection() throws Exception {
        var response=Map.of("error",Map.of("code","fixture_denied","message","denied"),"detail",17);
        try (var server=new TestPeer(req -> response)) {
            for (var row:rows()) {
                if (row.get("classification").equals("internal")) continue;
                try (var db=new PacificDBClient("127.0.0.1",server.port,"app")) {
                    db.security().useToken("fixture-token"); int before=server.frames.size();
                    var failure=assertThrows(PacificDBException.class, () -> invoke(db,row));
                    assertEquals("fixture_denied",failure.getCode()); assertEquals(response,failure.getResponse()); assertNull(failure.getCause());
                    assertEquals("app",db.getDatabase()); assertEquals(before+1,server.frames.size());
                }
            }
        }
    }

}
