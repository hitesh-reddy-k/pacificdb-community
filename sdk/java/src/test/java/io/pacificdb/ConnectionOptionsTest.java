package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;
import java.nio.file.Path;
import java.util.Map;
import java.util.List;
import static org.junit.jupiter.api.Assertions.*;

class ConnectionOptionsTest {
    @Test @SuppressWarnings("unchecked")
    void sharedUrlContractAndSecretFreeRepresentations() throws Exception {
        Map<String,Object> fixture = new ObjectMapper().readValue(Path.of("../contracts/connection-urls.json").toFile(), Map.class);
        for (Map<String,Object> row : (List<Map<String,Object>>)fixture.get("valid")) {
            ConnectionOptions options = ConnectionOptions.fromUrl((String)row.get("url"));
            if (row.containsKey("options")) options = options.withOverrides((Map<String,Object>)row.get("options"));
            Map<String,Object> expected = (Map<String,Object>)row.get("expected");
            assertEquals(expected.get("host"), options.getHost());
            assertEquals(expected.get("port"), options.getPort());
            assertEquals(expected.get("database"), options.getDatabase());
            assertEquals(expected.get("tls"), options.isTls());
            assertEquals(expected.get("timeoutMs"), options.getTimeoutMs());
            assertEquals(expected.get("poolSize"), options.getPoolSize());
            assertEquals(expected.get("userId"), options.getUserId());
            assertEquals(expected.get("caFile"), options.getCaFile());
            if (expected.containsKey("password")) assertFalse(options.toString().contains((String)expected.get("password")));
        }
        for (Map<String,Object> row : (List<Map<String,Object>>)fixture.get("invalid")) {
            var exception = assertThrows(PacificDBException.class, () -> {
                var options = ConnectionOptions.fromUrl((String)row.get("url"));
                if (row.containsKey("options")) options.withOverrides((Map<String,Object>)row.get("options"));
            });
            assertEquals(row.get("code"), exception.getCode());
            assertFalse(exception.toString().contains((String)row.get("url")));
            assertNull(exception.getCause());
        }
    }
}
