package site.ycsb.db;

import site.ycsb.*;
import java.io.*;
import java.net.Socket;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.SecureRandom;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import javax.net.SocketFactory;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManagerFactory;
import org.codehaus.jackson.JsonNode;
import org.codehaus.jackson.map.ObjectMapper;

/**
 * YCSB binding for PacificDB's persistent JSON protocol with verified read results.
 *
 * Properties:
 *   pacificdb.url        host:port (default 127.0.0.1:9000)
 *   pacificdb.database   default ycsb_db
 *   pacificdb.collection default ycsb_coll
 *   pacificdb.userId     default ycsb_user
 *   pacificdb.timeoutMs  default 15000
 *   pacificdb.tls        default true; set false only for isolated plaintext labs
 *   pacificdb.tls.ca/cert/key PEM paths for mTLS
 *   pacificdb.authTokenFile engine token file
 */
public class PacificDBClient extends DB {
  private Socket sock;
  private OutputStream out;
  private InputStream in;
  private SocketFactory socketFactory;
  private String host;
  private int port, timeout;
  private boolean tls;
  private String[] endpoints, tokenFiles;
  private int endpointIndex;
  private Set<String> allFields;
  private String userId, db, coll, readConsistency;
  private String token = "";
  private static final ObjectMapper JSON = new ObjectMapper();
  private static final AtomicInteger ERROR_LOGS = new AtomicInteger();

  private static void logError(String operation, Object detail) {
    if (ERROR_LOGS.getAndIncrement() < 12) {
      System.err.println("[PacificDBClient] " + operation + " failed: " + detail);
    }
  }

  private static byte[] pemBytes(String path, String type) throws IOException {
    String text = Files.readString(Paths.get(path));
    String begin = "-----BEGIN " + type + "-----";
    String end = "-----END " + type + "-----";
    int start = text.indexOf(begin);
    int finish = text.indexOf(end, start + begin.length());
    if (start < 0 || finish < 0) throw new IOException("invalid " + type + " PEM: " + path);
    String base64 = text.substring(start + begin.length(), finish).replaceAll("\\s", "");
    return Base64.getDecoder().decode(base64);
  }

  private static SSLSocketFactory tlsFactory(Properties p) throws Exception {
    String caPath = p.getProperty("pacificdb.tls.ca", "");
    String certPath = p.getProperty("pacificdb.tls.cert", "");
    String keyPath = p.getProperty("pacificdb.tls.key", "");
    if (caPath.isEmpty() || certPath.isEmpty() || keyPath.isEmpty()) {
      return (SSLSocketFactory) SSLSocketFactory.getDefault();
    }

    CertificateFactory certificates = CertificateFactory.getInstance("X.509");
    Certificate ca = certificates.generateCertificate(new ByteArrayInputStream(pemBytes(caPath, "CERTIFICATE")));
    Certificate client = certificates.generateCertificate(new ByteArrayInputStream(pemBytes(certPath, "CERTIFICATE")));
    PrivateKey key = KeyFactory.getInstance("RSA").generatePrivate(
        new PKCS8EncodedKeySpec(pemBytes(keyPath, "PRIVATE KEY")));

    KeyStore trustStore = KeyStore.getInstance(KeyStore.getDefaultType());
    trustStore.load(null, null);
    trustStore.setCertificateEntry("ca", ca);
    TrustManagerFactory trust = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
    trust.init(trustStore);

    char[] password = new char[0];
    KeyStore identity = KeyStore.getInstance("PKCS12");
    identity.load(null, password);
    identity.setKeyEntry("client", key, password, new Certificate[] {client, ca});
    KeyManagerFactory keys = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
    keys.init(identity, password);

    SSLContext context = SSLContext.getInstance("TLS");
    context.init(keys.getKeyManagers(), trust.getTrustManagers(), new SecureRandom());
    return context.getSocketFactory();
  }

  // Full JSON string escaping. YCSB's RandomByteIterator emits arbitrary byte values, so any
  // control character (< 0x20) must be escaped or the engine rejects the request as invalid_json.
  private static String jstr(String s) {
    StringBuilder b = new StringBuilder(s.length() + 8);
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      switch (c) {
        case '"':  b.append("\\\""); break;
        case '\\': b.append("\\\\"); break;
        case '\b': b.append("\\b"); break;
        case '\f': b.append("\\f"); break;
        case '\n': b.append("\\n"); break;
        case '\r': b.append("\\r"); break;
        case '\t': b.append("\\t"); break;
        default:
          if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
          else b.append(c);
      }
    }
    return b.toString();
  }

  private static String error(String response) throws IOException {
    JsonNode value = JSON.readTree(response);
    if (value == null || !value.isObject()) throw new IOException("invalid engine response");
    if (value.has("error")) return value.get("error").asText();
    if (value.has("success") && !value.get("success").asBoolean()) return "unsuccessful";
    return "";
  }

  // Retry only explicit rejection before mutation. A disconnected write has an
  // ambiguous outcome and must remain a benchmark error without proven deduplication.
  private String requestWithRetry(String json, boolean write) throws IOException {
    IOException last = new IOException("retry limit reached");
    for (int attempt = 0; attempt < 6; attempt++) {
      try {
        if (sock == null || sock.isClosed()) discoverLeader();
        String response = rpc(json);
        String failure = error(response);
        JsonNode value = JSON.readTree(response);
        boolean leadershipLost = failure.equals("not_leader")
            || failure.equals("term_changed_during_read")
            || (!failure.isEmpty() && value.has("isLeader") && !value.get("isLeader").asBoolean());
        if (leadershipLost) {
          sock.close();
          // Only not_leader proves a write was rejected before mutation. Close
          // other stale routes for the next operation, but retain this write's error.
          if (write && !failure.equals("not_leader")) return response;
        } else if (!failure.equals("server_busy") && !failure.equals("queue_full")) {
          return response;
        }
        last = new IOException("request rejected: " + failure);
      } catch (IOException failure) {
        if (sock != null) sock.close();
        if (write) throw failure;
        last = failure;
      }
      try { Thread.sleep(20L * (attempt + 1)); }
      catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        throw new InterruptedIOException("retry interrupted");
      }
    }
    throw last;
  }

  private Status writeWithRetry(String json) {
    try {
      String response = requestWithRetry(json, true);
      JsonNode value = JSON.readTree(response);
      String status = value.path("status").asText();
      if (error(response).isEmpty() &&
          ("ok".equals(status) || "updated".equals(status) || "deleted".equals(status)))
        return Status.OK;
      if ("not_found".equals(status)) return Status.NOT_FOUND;
      logError("write", response);
    } catch (Exception failure) { logError("write", failure); }
    return Status.ERROR;
  }

  private void discoverLeader() throws IOException {
    for (int round = 0; round < 10; round++) {
      for (int offset = 0; offset < endpoints.length; offset++) {
        int candidate = (endpointIndex + offset) % endpoints.length;
        String endpoint = endpoints[candidate];
        int separator = endpoint.lastIndexOf(':');
        host = endpoint.substring(0, separator);
        port = Integer.parseInt(endpoint.substring(separator + 1));
        if (tokenFiles.length > 0) {
          String file = tokenFiles[tokenFiles.length == 1 ? 0 : candidate];
          token = Files.readString(Paths.get(file)).trim();
        }
        try {
          connect();
          if (endpoints.length == 1) { endpointIndex = candidate; return; }
          JsonNode status = JSON.readTree(rpc("{\"action\":\"admin_raft_status\"}"));
          if (status.path("ok").asBoolean() && status.path("isLeader").asBoolean()
              && status.path("recoveryComplete").asBoolean()) {
            endpointIndex = candidate;
            return;
          }
          sock.close();
        } catch (IOException failure) {
          if (sock != null) sock.close();
        }
      }
      try { Thread.sleep(25L * (round + 1)); }
      catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
        throw new InterruptedIOException("leader discovery interrupted");
      }
    }
    throw new IOException("no ready leader among configured endpoints");
  }

  // ponytail: YCSB gives each worker its own client; one reconnecting socket per worker is the pool.
  private void connect() throws IOException {
    try { if (sock != null) sock.close(); } catch (Exception ignored) { }
    Socket next = socketFactory.createSocket();
    try {
      next.connect(new InetSocketAddress(host, port), timeout);
      next.setSoTimeout(timeout);
      if (tls) {
        SSLSocket sslSocket = (SSLSocket) next;
        SSLParameters parameters = sslSocket.getSSLParameters();
        parameters.setEndpointIdentificationAlgorithm("HTTPS");
        sslSocket.setSSLParameters(parameters);
        sslSocket.startHandshake();
      }
      next.setTcpNoDelay(true);
      sock = next;
      out = sock.getOutputStream();
      in = new BufferedInputStream(sock.getInputStream());
    } catch (IOException e) {
      try { next.close(); } catch (Exception ignored) { }
      throw e;
    }
  }

  @Override
  public void init() throws DBException {
    Properties p = getProperties();
    String url = p.getProperty("pacificdb.url", "127.0.0.1:9000");
    userId = p.getProperty("pacificdb.userId", "ycsb_user");
    db = p.getProperty("pacificdb.database", "ycsb_db");
    coll = p.getProperty("pacificdb.collection", "ycsb_coll");
    allFields = new LinkedHashSet<>();
    int fieldCount = Integer.parseInt(p.getProperty("fieldcount", "10"));
    String fieldPrefix = p.getProperty("fieldnameprefix", "field");
    for (int i = 0; i < fieldCount; i++) allFields.add(fieldPrefix + i);
    readConsistency = p.getProperty("pacificdb.readConsistency", "strong");
    timeout = Integer.parseInt(p.getProperty("pacificdb.timeoutMs", "15000"));
    tls = Boolean.parseBoolean(p.getProperty("pacificdb.tls", "true"));
    if (Boolean.parseBoolean(p.getProperty("pacificdb.binaryProtocol", "false")))
      throw new DBException("binaryProtocol needs a full response decoder; use JSON for validated benchmarks");
    boolean initializeSchema = Boolean.parseBoolean(p.getProperty("pacificdb.initializeSchema", "true"));
    String tokenFile = p.getProperty("pacificdb.authTokenFile", "");
    endpoints = p.getProperty("pacificdb.urls", url).split(",");
    tokenFiles = p.getProperty("pacificdb.authTokenFiles", tokenFile).split(",");
    if (tokenFiles.length == 1 && tokenFiles[0].isEmpty()) tokenFiles = new String[0];
    if (tokenFiles.length > 1 && tokenFiles.length != endpoints.length)
      throw new DBException("authTokenFiles must match urls");
    for (String endpoint : endpoints) {
      int separator = endpoint.lastIndexOf(':');
      if (separator <= 0 || separator == endpoint.length() - 1)
        throw new DBException("invalid endpoint");
    }
    try {
      if (!tokenFile.isEmpty()) token = Files.readString(Paths.get(tokenFile)).trim();
      socketFactory = tls ? tlsFactory(p) : SocketFactory.getDefault();
      discoverLeader();
      if (initializeSchema) {
        rpc("{\"action\":\"initUserSpace\",\"userId\":\"" + userId + "\"}");
        rpc("{\"action\":\"createDatabase\",\"userId\":\"" + userId + "\",\"dbName\":\"" + db + "\"}");
        rpc("{\"action\":\"createCollection\",\"userId\":\"" + userId + "\",\"dbName\":\"" + db
            + "\",\"collection\":\"" + coll + "\",\"name\":\"" + coll + "\"}");
      }
    } catch (Exception e) {
      throw new DBException(e);
    }
  }

  private String rpc(String json) throws IOException {
    if (!token.isEmpty() && json.endsWith("}")) {
      json = json.substring(0, json.length() - 1) + ",\"token\":\"" + jstr(token) + "\"}";
    }
    out.write((json + "\n").getBytes(StandardCharsets.UTF_8));
    out.flush();
    ByteArrayOutputStream line = new ByteArrayOutputStream();
    for (;;) {
      int next = in.read();
      if (next < 0) throw new EOFException("engine closed before response delimiter");
      if (next == '\n') return line.toString(StandardCharsets.UTF_8.name());
      if (line.size() >= 16 * 1024 * 1024) throw new IOException("response exceeds 16 MiB");
      line.write(next);
    }
  }

  private String docJson(String key, Map<String, ByteIterator> values) {
    StringBuilder b = new StringBuilder("{\"id\":\"").append(jstr(key)).append("\"");
    if (values != null)
      for (Map.Entry<String, ByteIterator> e : values.entrySet())
        b.append(",\"").append(jstr(e.getKey())).append("\":\"").append(jstr(e.getValue().toString())).append("\"");
    b.append("}");
    return b.toString();
  }

  private static void copyFields(JsonNode document, Set<String> fields,
                                 Map<String, ByteIterator> result) throws IOException {
    Iterator<String> names = fields.iterator();
    while (names.hasNext()) {
      String name = names.next();
      JsonNode value = document.get(name);
      if (value == null || !value.isTextual()) throw new IOException("missing/non-string field: " + name);
      result.put(name, new StringByteIterator(value.asText()));
    }
  }

  @Override
  public Status read(String table, String key, Set<String> fields, Map<String, ByteIterator> result) {
    try {
      String r = requestWithRetry("{\"action\":\"find\",\"userId\":\"" + userId + "\",\"dbName\":\"" + db
          + "\",\"collection\":\"" + coll + "\",\"filter\":{\"id\":\"" + jstr(key)
          + "\"},\"consistency\":\"" + jstr(readConsistency) + "\",\"limit\":1}", false);
      if (!error(r).isEmpty()) { logError("read", r); return Status.ERROR; }
      JsonNode data = JSON.readTree(r).get("data");
      if (data == null || !data.isArray()) return Status.ERROR;
      if (data.size() == 0) return Status.NOT_FOUND;
      if (data.size() != 1 || !key.equals(data.get(0).path("id").asText())) return Status.ERROR;
      result.clear();
      copyFields(data.get(0), fields == null ? allFields : fields, result);
      return Status.OK;
    } catch (Exception e) { logError("read", e); return Status.ERROR; }
  }

  @Override
  public Status scan(String table, String startkey, int recordcount, Set<String> fields,
                     Vector<HashMap<String, ByteIterator>> result) {
    return Status.NOT_IMPLEMENTED;
  }

  @Override
  public Status update(String table, String key, Map<String, ByteIterator> values) {
    return writeWithRetry("{\"action\":\"update\",\"userId\":\"" + userId + "\",\"dbName\":\"" + db
        + "\",\"collection\":\"" + coll + "\",\"filter\":{\"id\":\"" + jstr(key)
        + "\"},\"update\":{\"$set\":" + docJson(key, values) + "}}");
  }

  @Override
  public Status insert(String table, String key, Map<String, ByteIterator> values) {
    try {
      String lw = jstr("ycsb:" + userId + ":" + db + ":" + coll + ":" + key);
      return writeWithRetry("{\"action\":\"insert\",\"userId\":\"" + userId + "\",\"dbName\":\"" + db
          + "\",\"collection\":\"" + coll + "\",\"logicalWriteId\":\"" + lw + "\",\"idempotencyKey\":\"" + lw
          + "\",\"document\":" + docJson(key, values) + "}");
    } catch (Exception e) { return Status.ERROR; }
  }

  @Override
  public Status delete(String table, String key) {
    // the engine action is deleteOne (plain "delete" returns Unknown action)
    return writeWithRetry("{\"action\":\"deleteOne\",\"userId\":\"" + userId + "\",\"dbName\":\"" + db
        + "\",\"collection\":\"" + coll + "\",\"filter\":{\"id\":\"" + jstr(key) + "\"}}");
  }

  @Override
  public void cleanup() throws DBException {
    try { if (sock != null) sock.close(); } catch (Exception e) { }
  }
}
