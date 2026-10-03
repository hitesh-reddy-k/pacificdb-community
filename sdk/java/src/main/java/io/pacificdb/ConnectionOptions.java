package io.pacificdb;

import java.io.ByteArrayOutputStream;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Immutable connection settings. Parsing never opens a connection. */
public final class ConnectionOptions {
    private final Map<String, Object> values;
    private final Set<String> fixed;
    private static final Set<String> KEYS = Set.of("host", "port", "database", "userId", "tls",
        "timeoutMs", "poolSize", "caFile", "username", "password", "maxResponseBytes");

    private ConnectionOptions(Map<String, Object> values, Set<String> fixed) {
        this.values = Collections.unmodifiableMap(new LinkedHashMap<>(values));
        this.fixed = Set.copyOf(fixed);
        validate();
    }

    private static PacificDBException invalid() {
        return new PacificDBException("invalid_connection_url", "Invalid PacificDB connection URL or conflicting options", null);
    }

    private static String decode(String value) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        for (int i = 0; i < value.length();) {
            if (value.charAt(i) == '%') {
                if (i + 2 >= value.length()) throw invalid();
                int high = Character.digit(value.charAt(i + 1), 16), low = Character.digit(value.charAt(i + 2), 16);
                if (high < 0 || low < 0) throw invalid();
                bytes.write(high * 16 + low); i += 3;
            } else {
                int end = value.indexOf('%', i);
                if (end < 0) end = value.length();
                ByteBuffer encoded = StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                    .encode(java.nio.CharBuffer.wrap(value.substring(i, end)));
                while (encoded.hasRemaining()) bytes.write(encoded.get());
                i = end;
            }
        }
        String decoded = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes.toByteArray())).toString();
        if (decoded.isEmpty() || decoded.chars().anyMatch(c -> c < 32 || c == 127)) throw invalid();
        return decoded;
    }

    private static int integer(Object value, int max) {
        String text = String.valueOf(value);
        if (!text.matches("[0-9]+")) throw invalid();
        try {
            int number = Integer.parseInt(text);
            if (number < 1 || number > max) throw invalid();
            return number;
        } catch (NumberFormatException error) { throw invalid(); }
    }

    public static ConnectionOptions fromUrl(String url) {
        try {
            if (url == null || Pattern.compile("[\\s\\p{Cc}#]", Pattern.UNICODE_CHARACTER_CLASS).matcher(url).find()) throw invalid();
            Matcher match = Pattern.compile("(pacificdbs?)://([^/?#]+)(/[^?#]*)(?:\\?([^#]*))?").matcher(url);
            if (!match.matches()) throw invalid();
            Map<String, Object> values = new LinkedHashMap<>();
            Set<String> fixed = new HashSet<>(Set.of("host", "port", "database", "tls"));
            String database = decode(match.group(3).substring(1));
            if (database.contains("/") || database.contains("\\") || database.equals(".") || database.equals("..")) throw invalid();
            String address = match.group(2);
            if (address.contains("@")) {
                String[] pieces = address.split("@", -1);
                if (pieces.length != 2) throw invalid();
                String[] credentials = pieces[0].split(":", 2);
                if (credentials.length != 2) throw invalid();
                values.put("username", decode(credentials[0])); values.put("password", decode(credentials[1]));
                fixed.add("username"); fixed.add("password"); address = pieces[1];
            }
            Matcher host = Pattern.compile(address.startsWith("[") ? "\\[([^\\]]+)\\](?::([0-9]+))?" : "([A-Za-z0-9._-]+)(?::([0-9]+))?").matcher(address);
            if (!host.matches()) throw invalid();
            String hostname = host.group(1).toLowerCase(Locale.ROOT);
            if (address.startsWith("[") && (!hostname.matches("[0-9a-f:.]+") || !(InetAddress.getByName(hostname) instanceof Inet6Address))) throw invalid();
            values.put("host", hostname); values.put("port", host.group(2) == null ? 9000 : integer(host.group(2), 65535));
            values.put("database", database); values.put("tls", match.group(1).equals("pacificdbs"));
            values.put("userId", "system"); values.put("timeoutMs", 30000); values.put("poolSize", 16);
            values.put("maxResponseBytes", 64 * 1024 * 1024);
            String query = match.group(4);
            if (query != null) {
                if (query.isEmpty()) throw invalid();
                Set<String> seen = new HashSet<>();
                for (String pair : query.split("&", -1)) {
                    String[] pieces = pair.split("=", 2);
                    if (pieces.length != 2) throw invalid();
                    String key = decode(pieces[0].replace("+", "%20"));
                    String value = decode(pieces[1].replace("+", "%20"));
                    if (!Set.of("userId", "timeoutMs", "poolSize", "caFile").contains(key) || !seen.add(key)) throw invalid();
                    values.put(key, key.equals("timeoutMs") ? integer(value, 1800000) : key.equals("poolSize") ? integer(value, 32) : value);
                    fixed.add(key);
                }
            }
            return new ConnectionOptions(values, fixed);
        } catch (Exception error) { throw invalid(); }
    }

    public ConnectionOptions withOverrides(Map<String, Object> options) {
        if (options == null) throw invalid();
        Map<String, Object> merged = new LinkedHashMap<>(values);
        options.forEach((key, value) -> {
            if (!KEYS.contains(key) || fixed.contains(key) && !Objects.equals(values.get(key), value)) throw invalid();
            merged.put(key, value);
        });
        return new ConnectionOptions(merged, fixed);
    }

    static ConnectionOptions legacy(String host, int port, String user, String database, boolean tls, int timeout) {
        Map<String, Object> values = new LinkedHashMap<>();
        values.put("host", host); values.put("port", port); values.put("userId", user); values.put("database", database);
        values.put("tls", tls); values.put("timeoutMs", timeout); values.put("poolSize", 16); values.put("maxResponseBytes", 64 * 1024 * 1024);
        return new ConnectionOptions(values, Set.of());
    }

    private void validate() {
        for (String field : List.of("host", "userId", "database"))
            if (!(values.get(field) instanceof String) || !field.equals("database") && ((String)values.get(field)).isEmpty()) throw invalid();
        if (getHost().matches(".*[\\s/@\\p{Cc}].*") || !(values.get("tls") instanceof Boolean)) throw invalid();
        for (String field : List.of("port", "timeoutMs", "poolSize", "maxResponseBytes"))
            if (!(values.get(field) instanceof Integer)) throw invalid();
        integer(values.get("port"), 65535); integer(values.get("timeoutMs"), 1800000);
        integer(values.get("poolSize"), 32); integer(values.get("maxResponseBytes"), 64 * 1024 * 1024);
        if (values.containsKey("caFile") && (!(values.get("caFile") instanceof String) || ((String)values.get("caFile")).isEmpty() || !isTls())) throw invalid();
        if (values.containsKey("username") || values.containsKey("password"))
            for (String field : List.of("username", "password"))
                if (!(values.get(field) instanceof String) || ((String)values.get(field)).isEmpty()) throw invalid();
    }
    public String getHost() { return (String)values.get("host"); }
    public int getPort() { return (Integer)values.get("port"); }
    public String getDatabase() { return (String)values.get("database"); }
    public String getUserId() { return (String)values.get("userId"); }
    public boolean isTls() { return (Boolean)values.get("tls"); }
    public int getTimeoutMs() { return (Integer)values.get("timeoutMs"); }
    public int getPoolSize() { return (Integer)values.get("poolSize"); }
    public String getCaFile() { return (String)values.get("caFile"); }
    public int getMaxResponseBytes() { return (Integer)values.get("maxResponseBytes"); }
    String username() { return (String)values.get("username"); }
    String password() { return (String)values.get("password"); }
    @Override public String toString() { return "ConnectionOptions(host=" + getHost() + ", port=" + getPort() + ", tls=" + isTls() + ")"; }
}
