package io.pacificdb;

import java.util.Map;

/** Beginner entry point; operations live on the returned AutoCloseable client. */
public final class PacificDB {
    private PacificDB() { }
    public static PacificDBClient fromUrl(String url) { return fromUrl(ConnectionOptions.fromUrl(url)); }
    public static PacificDBClient fromUrl(String url, Map<String, Object> options) {
        return fromUrl(ConnectionOptions.fromUrl(url).withOverrides(options));
    }
    public static PacificDBClient fromUrl(ConnectionOptions options) { return new PacificDBClient(options); }
    public static PacificDBClient connect(String url) { return connect(ConnectionOptions.fromUrl(url)); }
    public static PacificDBClient connect(String url, Map<String, Object> options) {
        return connect(ConnectionOptions.fromUrl(url).withOverrides(options));
    }
    public static PacificDBClient connect(ConnectionOptions options) {
        PacificDBClient client = fromUrl(options);
        try { client.request(Map.of("action", "ping")); return client; }
        catch (RuntimeException error) { client.close(); throw error; }
    }
}
