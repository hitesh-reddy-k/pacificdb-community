package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import javax.net.SocketFactory;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;

/** Direct database client. Close it, or use try-with-resources, when finished. */
public final class PacificDBClient implements AutoCloseable {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ConnectionOptions options;
    private final ConnectionPool pool;
    private final Set<String> secrets = new HashSet<>();
    private String database, token = "", authState = "new";
    private boolean closed;

    public PacificDBClient(String host, int port, String userId, String database, boolean tls, int timeoutMs) {
        this(ConnectionOptions.legacy(host, port, userId, database, tls, timeoutMs), null);
    }
    PacificDBClient(String host, int port, String userId, String database, boolean tls, int timeoutMs, SocketFactory factory) {
        this(ConnectionOptions.legacy(host, port, userId, database, tls, timeoutMs), factory);
    }
    public PacificDBClient(String host, int port, String database) { this(host, port, "system", database, false, 30000); }
    public PacificDBClient(ConnectionOptions options) { this(options, null); }
    private PacificDBClient(ConnectionOptions options, SocketFactory factory) {
        this.options = Objects.requireNonNull(options);
        this.database = options.getDatabase();
        if (options.password() != null) secrets.add(options.password());
        this.pool = new ConnectionPool(options, factory);
    }

    public synchronized String getDatabase() { return database; }
    public String getHost() { return options.getHost(); }
    public int getPort() { return options.getPort(); }
    public String getUserId() { return options.getUserId(); }
    @Override public String toString() { return "PacificDBClient(" + options + ")"; }
    @Override public void close() {
        synchronized (this) { closed = true; notifyAll(); }
        pool.close();
    }

    @SuppressWarnings("unchecked")
    public Map<String, Object> request(Map<String, Object> command) {
        Object value = requestValue(command);
        if (!(value instanceof Map)) throw new PacificDBException("unexpected_response_shape", "Expected an object response; use requestValue for arrays", null);
        return (Map<String, Object>)value;
    }
    public Object requestValue(Map<String, Object> command) { return scopedRequester().apply(command); }

    static final class ScopedRequester implements Function<Map<String, Object>, Object> {
        final String database;
        private final Map<String,Object> scope;
        private final Function<Map<String,Object>,Object> request;
        ScopedRequester(Map<String,Object> scope, Function<Map<String,Object>,Object> request) {
            this.scope=scope; this.database=(String)scope.get("dbName"); this.request=request;
        }
        public Object apply(Map<String,Object> command) { return request.apply(command); }
        int wireBytes(Map<String,Object> command) {
            Map<String,Object> payload;
            synchronized(scope) { payload=new LinkedHashMap<>(scope); }
            payload.putAll(command); return encode(payload).length+1;
        }
    }
    ScopedRequester scopedRequester() {
        Map<String, Object> scope = new LinkedHashMap<>();
        synchronized (this) {
            scope.put("userId", options.getUserId()); scope.put("dbName", database);
            if (!token.isEmpty()) scope.put("token", token);
        }
        return new ScopedRequester(scope, command -> {
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(options.getTimeoutMs());
            try {
                if (command == null) throw new PacificDBException("invalid_request");
                Map<String, Object> payload;
                synchronized (scope) { payload = new LinkedHashMap<>(scope); }
                payload.putAll(command);
                learnSecrets(payload);
                // Capture caller-owned nested maps/lists before authentication or waiting for a lease.
                byte[] snapshot = encode(payload);
                payload = JSON.readValue(snapshot, Map.class);
                if (!"security_authenticate".equals(payload.get("action"))) {
                    String authenticatedToken = ensureAuthenticated(deadline);
                    synchronized (scope) {
                        if (!authenticatedToken.isEmpty()) scope.putIfAbsent("token", authenticatedToken);
                        if (scope.containsKey("token")) payload.putIfAbsent("token", scope.get("token"));
                    }
                }
                return send(payload, deadline);
            } catch (Exception error) {
                throw safeError(error);
            }
        });
    }

    private synchronized void learnSecrets(Map<String, Object> payload) { learnSecrets(payload, 0); }
    private void learnSecrets(Object value, int depth) {
        if (depth > 256) return; // Invalid cyclic/deep commands are refused by encode before any I/O.
        if (value instanceof Map) {
            ((Map<?, ?>)value).forEach((key, item) -> {
                if (String.valueOf(key).toLowerCase(Locale.ROOT).matches(".*(password|token|authorization).*") && item instanceof String && !((String)item).isEmpty())
                    secrets.add((String)item);
                learnSecrets(item, depth + 1);
            });
        } else if (value instanceof Collection) ((Collection<?>)value).forEach(item -> learnSecrets(item, depth + 1));
    }
    private static void validateJson(Object value, int depth) {
        if (depth > 256) throw new PacificDBException("invalid_request", "Command nesting exceeds 256 levels or contains a cycle", null);
        if (value instanceof Double && !Double.isFinite((Double)value) || value instanceof Float && !Float.isFinite((Float)value))
            throw new PacificDBException("invalid_request");
        if (value instanceof Map) ((Map<?, ?>)value).values().forEach(item -> validateJson(item, depth + 1));
        else if (value instanceof Collection) ((Collection<?>)value).forEach(item -> validateJson(item, depth + 1));
    }
    private static byte[] encode(Map<String, Object> payload) {
        try { validateJson(payload, 0); return JSON.writeValueAsBytes(payload); }
        catch (Exception error) { throw new PacificDBException("invalid_request", "Command must contain valid JSON values", null); }
    }

    private Object send(Map<String, Object> payload, long deadline) {
        learnSecrets(payload);
        byte[] encoded = encode(payload), wire = Arrays.copyOf(encoded, encoded.length + 1);
        wire[encoded.length] = '\n';
        Object value = pool.request(wire, deadline);
        if (value instanceof Map) {
            Map<?, ?> response = (Map<?, ?>)value;
            Object error = response.get("error");
            if (error != null && !Boolean.FALSE.equals(error) && !"".equals(error)) {
                String code = error instanceof Map ? String.valueOf(((Map<?, ?>)error).getOrDefault("code", null)) : String.valueOf(error);
                Object message = error instanceof Map ? ((Map<?, ?>)error).get("message") : response.get("message");
                if (code.equals("null")) code = "engine_error";
                throw new PacificDBException(code, message == null ? code : String.valueOf(message), value);
            }
        }
        return value;
    }

    private String ensureAuthenticated(long deadline) throws InterruptedException {
        Map<String, Object> auth;
        synchronized (this) {
            if (closed) throw new PacificDBException("client_closed");
            if (options.username() == null) return token;
            while (authState.equals("pending") && !closed) {
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0) throw new PacificDBException("request_timeout");
                TimeUnit.NANOSECONDS.timedWait(this, remaining);
            }
            if (closed || authState.equals("failed")) throw new PacificDBException("client_closed");
            if (authState.equals("ready")) return token;
            authState = "pending";
            auth = new LinkedHashMap<>(); auth.put("userId", options.getUserId()); auth.put("dbName", database);
            auth.put("action", "security_authenticate"); auth.put("username", options.username()); auth.put("password", options.password());
        }
        try {
            Object response = send(auth, deadline);
            String receivedToken = authenticationToken(response);
            synchronized (this) {
                if (closed) throw new PacificDBException("client_closed");
                token = receivedToken; secrets.add(token); authState = "ready"; notifyAll(); return token;
            }
        } catch (RuntimeException error) {
            synchronized (this) { authState = "failed"; }
            close(); throw error;
        }
    }
    private static String authenticationToken(Object response) {
        Object value = response instanceof Map ? ((Map<?, ?>)response).get("token") : null;
        if (!(value instanceof String) || ((String)value).isEmpty()) throw new PacificDBException("invalid_response", "Invalid authentication response", null);
        return (String)value;
    }
    public Map<String, Object> authenticate(String username, String password) {
        if (username == null || username.isEmpty() || password == null || password.isEmpty()) throw new PacificDBException("invalid_credentials");
        Map<String, Object> result = request(Map.of("action", "security_authenticate", "username", username, "password", password));
        String receivedToken = authenticationToken(result);
        synchronized (this) { if (closed) throw new PacificDBException("client_closed"); token = receivedToken; secrets.add(token); }
        return result;
    }

    private static String name(String value, String kind) {
        if (value == null || value.isEmpty()) throw new PacificDBException("invalid_" + kind, "A non-empty " + kind + " name is required", null);
        return value;
    }
    Object databaseRequest(Map<String, Object> command) {
        Function<Map<String, Object>, Object> request;
        synchronized (this) {
            if (database.isEmpty()) throw new PacificDBException("database_required", "Select or create a database first", null);
            request = scopedRequester();
        }
        return request.apply(command);
    }
    Object collectionRequest(String collection, String action, Map<String, Object> fields) {
        name(collection, "collection");
        Map<String, Object> command = new LinkedHashMap<>(fields);
        command.put("action", action); command.put("collection", collection);
        return databaseRequest(command);
    }
    private static <T> T required(T value) {
        if (value == null) throw new PacificDBException("invalid_request", "JSON arguments cannot be null", null);
        return value;
    }
    synchronized void useToken(String value) {
        if (value == null || value.isEmpty()) throw new PacificDBException("invalid_token");
        if (closed) throw new PacificDBException("client_closed");
        token = value; secrets.add(value);
    }
    public Operations.Indexes indexes() { return new Operations.Indexes(this); }
    public Operations.Vectors vectors() { return new Operations.Vectors(this); }
    public Operations.Media media() { return new Operations.Media(this); }
    public Operations.Backups backups() { return new Operations.Backups(this); }
    public Operations.Security security() { return new Operations.Security(this); }
    public Operations.Admin admin() { return new Operations.Admin(this); }
    public Object capabilities() { return requestValue(Map.of("action", "community_capabilities")); }
    public Object createDatabase() { return createDatabase(getDatabase()); }
    public Object createDatabase(String name) { return createDatabase(name, "binary"); }
    public Object createDatabase(String name, String dbType) {
        name(name, "database"); name(dbType, "database_type");
        Object response = requestValue(Map.of("action", "createDatabase", "dbName", name, "dbType", dbType));
        synchronized (this) { database = name; }
        return response;
    }
    @SuppressWarnings("unchecked") public List<String> listDatabases() {
        Object response = requestValue(Map.of("action", "listDatabases"));
        if (!(response instanceof List) || ((List<?>)response).stream().anyMatch(value -> !(value instanceof String)))
            throw new PacificDBException("unexpected_response_shape", "Expected an array of database names", null);
        return (List<String>)response;
    }
    public List<String> useDatabase(String name) {
        name(name, "database");
        List<String> names = listDatabases();
        if (!names.contains(name)) throw new PacificDBException("database_not_found");
        synchronized (this) { database = name; }
        return names;
    }
    public Object dropDatabase() { return dropDatabase(getDatabase()); }
    public Object dropDatabase(String name) {
        name(name, "database");
        Object response = requestValue(Map.of("action", "dropDatabase", "dbName", name));
        synchronized (this) { if (database.equals(name)) database = ""; }
        return response;
    }
    public Object createCollection(String name) { return collectionRequest(name, "createCollection", Map.of()); }
    public Object listCollections() { return databaseRequest(Map.of("action", "listCollections")); }
    public Object dropCollection(String name) { return collectionRequest(name, "dropCollection", Map.of()); }
    public Object insert(String collection, Map<String, Object> document) { return collectionRequest(collection, "insert", Map.of("data", required(document))); }
    public Object insertMany(String collection, List<? extends Map<String, Object>> documents) {
        required(documents).forEach(PacificDBClient::required);
        return collectionRequest(collection, "insertMany", Map.of("data", documents));
    }
    public Object find(String collection) { return find(collection, Map.of()); }
    public Object find(String collection, Map<String, Object> filter) { return find(collection, filter, -1, 0); }
    public Object find(String collection, Map<String, Object> filter, int limit, int offset) {
        if (limit < -1 || offset < 0) throw new PacificDBException("invalid_pagination", "limit must be -1 or nonnegative; offset must be nonnegative", null);
        return collectionRequest(collection, "find", Map.of("filter", required(filter), "limit", limit, "offset", offset));
    }
    public Map<String, Object> findOne(String collection) { return findOne(collection, Map.of()); }
    @SuppressWarnings("unchecked") public Map<String, Object> findOne(String collection, Map<String, Object> filter) {
        Object response = find(collection, filter, 1, 0);
        Object rows = response instanceof Map ? ((Map<?, ?>)response).get("data") : response;
        if (!(rows instanceof List)) throw new PacificDBException("unexpected_response_shape");
        List<?> documents = (List<?>)rows;
        if (documents.isEmpty()) return null;
        if (!(documents.get(0) instanceof Map)) throw new PacificDBException("unexpected_response_shape");
        return (Map<String, Object>)documents.get(0);
    }
    public Object count(String collection) { return count(collection, Map.of()); }
    public Object count(String collection, Map<String, Object> filter) { return collectionRequest(collection, "count", Map.of("filter", required(filter))); }
    public Object aggregate(String collection, List<? extends Map<String, Object>> pipeline) { return collectionRequest(collection, "aggregate", Map.of("pipeline", required(pipeline))); }
    public Object explain(String collection) { return explain(collection, Map.of()); }
    public Object explain(String collection, Map<String, Object> filter) { return collectionRequest(collection, "explain", Map.of("filter", required(filter))); }
    public Object updateOne(String collection, Map<String, Object> filter, Map<String, Object> update) { return collectionRequest(collection, "updateOne", Map.of("filter", required(filter), "update", required(update))); }
    public Object updateMany(String collection, Map<String, Object> filter, Map<String, Object> update) { return collectionRequest(collection, "updateMany", Map.of("filter", required(filter), "update", required(update))); }
    public Object deleteOne(String collection, Map<String, Object> filter) { return collectionRequest(collection, "deleteOne", Map.of("filter", required(filter))); }
    public Object deleteMany(String collection, Map<String, Object> filter) { return collectionRequest(collection, "deleteMany", Map.of("filter", required(filter))); }
    public Object bulkWrite(String collection, List<? extends Map<String, Object>> operations) {
        required(operations).forEach(PacificDBClient::required);
        return collectionRequest(collection, "bulkWrite", Map.of("ops", operations));
    }

    private synchronized PacificDBException safeError(Exception error) {
        if (error instanceof InterruptedException) Thread.currentThread().interrupt();
        if (!(error instanceof PacificDBException)) return new PacificDBException(error instanceof InterruptedException ? "request_interrupted" : "invalid_request");
        PacificDBException failure = (PacificDBException)error;
        return new PacificDBException(redact(failure.getCode()), redact(failure.getMessage()), sanitize(failure.getResponse()));
    }
    private String redact(String value) {
        if (value == null) return null;
        value = value.replaceAll("pacificdbs?://[^\\s/]*@", "pacificdb://[redacted]@");
        for (String secret : secrets) value = value.replace(secret, "[redacted]");
        return value;
    }
    private Object sanitize(Object value) {
        if (value instanceof String) return redact((String)value);
        if (value instanceof Map) {
            Map<String, Object> result = new LinkedHashMap<>();
            ((Map<?, ?>)value).forEach((key, item) -> result.put(redact(String.valueOf(key)),
                String.valueOf(key).toLowerCase(Locale.ROOT).matches(".*(password|token|authorization).*") ? "[redacted]" : sanitize(item)));
            return result;
        }
        if (value instanceof List) {
            List<Object> result = new ArrayList<>(); ((List<?>)value).forEach(item -> result.add(sanitize(item))); return result;
        }
        return value;
    }
}
