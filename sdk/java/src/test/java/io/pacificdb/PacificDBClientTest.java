package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.net.ServerSocket;
import java.net.Socket;
import java.security.KeyStore;
import java.security.SecureRandom;
import java.security.cert.Certificate;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.Executors;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLServerSocket;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManagerFactory;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertNull;

class PacificDBClientTest {
    private SSLContext[] testTlsContexts() throws Exception { return testTlsContexts(null); }
    private SSLContext[] testTlsContexts(Path caFile) throws Exception {
        // Generate a fresh test-only certificate so no private key is checked in.
        Path keystorePath = Files.createTempFile("pacificdb-tls-test-", ".p12");
        Files.delete(keystorePath);
        char[] password = "test-only-password".toCharArray();
        KeyStore serverKeys = KeyStore.getInstance("PKCS12");
        try {
            String executable = System.getProperty("os.name").startsWith("Windows")
                ? "keytool.exe" : "keytool";
            Path keytool = Path.of(System.getProperty("java.home"), "bin", executable);
            Process process = new ProcessBuilder(keytool.toString(), "-genkeypair",
                "-alias", "server", "-keyalg", "EC", "-keysize", "256",
                "-keystore", keystorePath.toString(), "-storetype", "PKCS12",
                "-storepass", String.valueOf(password), "-keypass", String.valueOf(password),
                "-dname", "CN=localhost", "-ext", "SAN=dns:localhost",
                "-validity", "3650", "-noprompt").redirectErrorStream(true).start();
            if (!process.waitFor(15, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                throw new IOException("keytool timed out");
            }
            String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8);
            if (process.exitValue() != 0) throw new IOException("keytool failed: " + output);
            try (var input = Files.newInputStream(keystorePath)) {
                serverKeys.load(input, password);
            }
        } finally {
            Files.deleteIfExists(keystorePath);
        }
        Certificate certificate = serverKeys.getCertificate("server");
        if (caFile != null) Files.writeString(caFile, "-----BEGIN CERTIFICATE-----\n" + java.util.Base64.getMimeEncoder(64, new byte[] {10}).encodeToString(certificate.getEncoded()) + "\n-----END CERTIFICATE-----\n");
        KeyManagerFactory keyManagers = KeyManagerFactory.getInstance(
            KeyManagerFactory.getDefaultAlgorithm());
        keyManagers.init(serverKeys, password);
        SSLContext serverContext = SSLContext.getInstance("TLS");
        serverContext.init(keyManagers.getKeyManagers(), null, new SecureRandom());

        KeyStore trusted = KeyStore.getInstance("PKCS12");
        trusted.load(null, null);
        trusted.setCertificateEntry("server", certificate);
        TrustManagerFactory trustManagers = TrustManagerFactory.getInstance(
            TrustManagerFactory.getDefaultAlgorithm());
        trustManagers.init(trusted);
        SSLContext clientContext = SSLContext.getInstance("TLS");
        clientContext.init(null, trustManagers.getTrustManagers(), new SecureRandom());
        return new SSLContext[] { serverContext, clientContext };
    }

    @Test
    void sendsAndReceivesOneJsonLine() throws Exception {
        ObjectMapper json = new ObjectMapper();
        try (ServerSocket server = new ServerSocket(0)) {
            var executor = Executors.newSingleThreadExecutor();
            var future = executor.submit(() -> {
                try (Socket socket = server.accept()) {
                    Map<?, ?> request = json.readValue(
                        new BufferedReader(new InputStreamReader(socket.getInputStream())).readLine(),
                        Map.class);
                    new PrintWriter(socket.getOutputStream(), true).println(
                        json.writeValueAsString(Map.of("ok", true, "action", request.get("action"))));
                }
                return null;
            });
            try (PacificDBClient client = new PacificDBClient("127.0.0.1", server.getLocalPort(), "app")) {
            assertEquals("ping", client.request(Map.of("action", "ping")).get("action"));
            future.get();
            }
            executor.shutdown();
        }
    }

    @Test
    void verifiesTlsServerHostnameBeforeSendingCredentials() throws Exception {
        SSLContext[] contexts = testTlsContexts();
        try (SSLServerSocket server = (SSLServerSocket) contexts[0]
                 .getServerSocketFactory().createServerSocket(0)) {
            var executor = Executors.newSingleThreadExecutor();
            try {
                var future = executor.submit(() -> {
                    try (SSLSocket socket = (SSLSocket) server.accept()) {
                        try {
                            new BufferedReader(new InputStreamReader(socket.getInputStream(),
                                StandardCharsets.UTF_8)).readLine();
                        } catch (IOException expectedHandshakeFailure) {
                            // The client rejects a trusted certificate for the wrong host.
                        }
                    }
                    return null;
                });
                try (PacificDBClient client = new PacificDBClient("127.0.0.1", server.getLocalPort(),
                    "system", "app", true, 3_000, contexts[1].getSocketFactory())) {
                var error = assertThrows(PacificDBException.class,
                    () -> client.request(Map.of("action", "ping", "token", "secret")));
                assertEquals("tls_error", error.getCode()); assertNull(error.getCause());
                future.get(5, TimeUnit.SECONDS);
                }
            } finally {
                executor.shutdownNow();
            }
        }
    }

    @Test
    void acceptsTrustedTlsServerWithMatchingHostname() throws Exception {
        SSLContext[] contexts = testTlsContexts();
        try (SSLServerSocket server = (SSLServerSocket) contexts[0]
                 .getServerSocketFactory().createServerSocket(0)) {
            var executor = Executors.newSingleThreadExecutor();
            try {
                var future = executor.submit(() -> {
                    try (SSLSocket socket = (SSLSocket) server.accept()) {
                        new BufferedReader(new InputStreamReader(socket.getInputStream(),
                            StandardCharsets.UTF_8)).readLine();
                        new PrintWriter(socket.getOutputStream(), true).println("{\"ok\":true}");
                    }
                    return null;
                });
                try (PacificDBClient client = new PacificDBClient("localhost", server.getLocalPort(),
                    "system", "app", true, 3_000, contexts[1].getSocketFactory())) {
                assertEquals(true, client.request(Map.of("action", "ping")).get("ok"));
                future.get(5, TimeUnit.SECONDS);
                }
            } finally {
                executor.shutdownNow();
            }
        }
    }
    @Test
    void customCaUrlSucceedsAndDefaultTrustRejectsPrivateCertificateBeforeAuth() throws Exception {
        Path ca = Files.createTempFile("pacificdb-ca-", ".pem");
        SSLContext[] contexts = testTlsContexts(ca);
        try {
            for (boolean trusted : new boolean[] {true, false}) {
                try (SSLServerSocket server = (SSLServerSocket)contexts[0].getServerSocketFactory().createServerSocket(0)) {
                    var workers = Executors.newSingleThreadExecutor();
                    var received = new java.util.concurrent.atomic.AtomicInteger();
                    try {
                        var peer = workers.submit(() -> {
                            try (SSLSocket socket = (SSLSocket)server.accept()) {
                                var reader = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
                                var writer = new PrintWriter(socket.getOutputStream(), true);
                                for (int i = 0; i < 2; i++) {
                                    String line = reader.readLine();
                                    if (line == null) break;
                                    received.incrementAndGet();
                                    writer.println(i == 0 ? "{\"token\":\"private-token\",\"_pacificdb_connection_keepalive\":true}" : "{\"ok\":true}");
                                }
                            } catch (IOException failure) { if (trusted) throw failure; }
                            return null;
                        });
                        String url = "pacificdbs://demo:private-password@localhost:" + server.getLocalPort() + "/app";
                        if (trusted) {
                            try (var db = PacificDB.connect(url, Map.of("caFile", ca.toString()))) { assertEquals("app", db.getDatabase()); }
                            assertEquals(2, received.get());
                        } else {
                            var error = assertThrows(PacificDBException.class, () -> PacificDB.connect(url));
                            assertEquals("tls_error", error.getCode()); assertNull(error.getCause());
                            assertEquals(0, received.get());
                        }
                        peer.get(5, TimeUnit.SECONDS);
                    } finally { workers.shutdownNow(); }
                }
            }
        } finally { Files.deleteIfExists(ca); }
    }

}
