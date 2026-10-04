package io.pacificdb;

import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import javax.net.SocketFactory;
import javax.net.ssl.*;
import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.charset.*;
import java.nio.file.*;
import java.security.KeyStore;
import java.security.cert.CertificateFactory;
import java.util.*;
import java.util.concurrent.*;

/** One request per leased socket; no reconnect-and-replay of a sent command. */
final class ConnectionPool implements AutoCloseable {
    private static final ObjectMapper JSON = new ObjectMapper().enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);
    private final ConnectionOptions options;
    private final SocketFactory factory;
    private final ArrayDeque<Socket> idle = new ArrayDeque<>();
    private final Set<Socket> sockets = new HashSet<>();
    private final ScheduledThreadPoolExecutor timer;
    private int leased;
    private boolean closed;

    ConnectionPool(ConnectionOptions options, SocketFactory injected) {
        this.options = options;
        this.factory = injected == null ? socketFactory(options) : injected;
        timer = new ScheduledThreadPoolExecutor(1, task -> {
            Thread thread = new Thread(task, "pacificdb-deadlines"); thread.setDaemon(true); return thread;
        });
        timer.setRemoveOnCancelPolicy(true);
        timer.setExecuteExistingDelayedTasksAfterShutdownPolicy(false);
    }

    private static SocketFactory socketFactory(ConnectionOptions options) {
        if (!options.isTls()) return SocketFactory.getDefault();
        if (options.getCaFile() == null) return SSLSocketFactory.getDefault();
        try (InputStream input = Files.newInputStream(Path.of(options.getCaFile()))) {
            KeyStore trust = KeyStore.getInstance(KeyStore.getDefaultType()); trust.load(null, null);
            int count = 0;
            for (var certificate : CertificateFactory.getInstance("X.509").generateCertificates(input))
                trust.setCertificateEntry("ca-" + count++, certificate);
            if (count == 0) throw new PacificDBException("invalid_ca_file");
            TrustManagerFactory managers = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            managers.init(trust);
            SSLContext context = SSLContext.getInstance("TLS"); context.init(null, managers.getTrustManagers(), null);
            return context.getSocketFactory();
        } catch (Exception error) { throw new PacificDBException("invalid_ca_file", "Cannot load TLS trust certificates", null); }
    }

    private static int remaining(long deadline) {
        long left = deadline - System.nanoTime();
        if (left <= 0) throw new PacificDBException("request_timeout");
        return (int)Math.min(Integer.MAX_VALUE, Math.max(1, TimeUnit.NANOSECONDS.toMillis(left) + 1));
    }

    private synchronized Socket acquire(long deadline) throws IOException, InterruptedException {
        while (!closed && leased >= options.getPoolSize()) wait(remaining(deadline));
        if (closed) throw new PacificDBException("client_closed");
        remaining(deadline);
        Socket socket = idle.isEmpty() ? factory.createSocket() : idle.removeFirst();
        leased++; sockets.add(socket);
        return socket;
    }

    private synchronized void release(Socket socket, boolean reusable) {
        leased--;
        if (!closed && reusable && !socket.isClosed()) idle.addLast(socket);
        else { sockets.remove(socket); closeSocket(socket); }
        notifyAll();
    }

    private static void closeSocket(Socket socket) { try { socket.close(); } catch (IOException ignored) { } }

    Object request(byte[] wire, long deadline) {
        Socket socket = null;
        ScheduledFuture<?> expiry = null;
        boolean reusable = false, sent = false;
        Object deadlineLock = new Object();
        boolean[] pending = {true};
        try {
            socket = acquire(deadline);
            Socket leasedSocket = socket;
            expiry = timer.schedule(() -> {
                synchronized (deadlineLock) { if (pending[0]) closeSocket(leasedSocket); }
            }, remaining(deadline), TimeUnit.MILLISECONDS);
            if (!socket.isConnected()) {
                // The OS resolver can block before connect; the socket and all subsequent I/O are deadline bounded.
                socket.connect(new InetSocketAddress(options.getHost(), options.getPort()), remaining(deadline));
                socket.setTcpNoDelay(true);
                socket.setSoTimeout(remaining(deadline));
                if (options.isTls()) {
                    if (!(socket instanceof SSLSocket)) throw new PacificDBException("tls_error");
                    SSLSocket ssl = (SSLSocket)socket;
                    SSLParameters parameters = ssl.getSSLParameters();
                    parameters.setEndpointIdentificationAlgorithm("HTTPS");
                    parameters.setProtocols(Arrays.stream(ssl.getSupportedProtocols())
                        .filter(protocol -> protocol.equals("TLSv1.2") || protocol.equals("TLSv1.3")).toArray(String[]::new));
                    if (!options.getHost().contains(":") && !options.getHost().matches("[0-9.]+"))
                        parameters.setServerNames(List.of(new SNIHostName(options.getHost())));
                    ssl.setSSLParameters(parameters); ssl.startHandshake();
                }
            }
            remaining(deadline);
            sent = true;
            socket.getOutputStream().write(wire); socket.getOutputStream().flush();
            Object value = read(socket, deadline);
            remaining(deadline);
            if (value instanceof Map) {
                Map<?, ?> response = (Map<?, ?>)value;
                reusable = Boolean.TRUE.equals(response.remove("_pacificdb_connection_keepalive"));
                if (Boolean.TRUE.equals(response.remove("_pacificdb_connection_close"))) reusable = false;
            }
            return value;
        } catch (Exception error) {
            String code;
            synchronized (this) {
                code = closed ? "client_closed" : System.nanoTime() >= deadline || error instanceof SocketTimeoutException ? "request_timeout"
                    : error instanceof PacificDBException ? ((PacificDBException)error).getCode()
                    : error instanceof SSLException ? "tls_error" : error instanceof InterruptedException ? "request_interrupted" : "connection_lost";
            }
            if (error instanceof InterruptedException) Thread.currentThread().interrupt();
            throw new PacificDBException(code, code + (sent ? "; a sent write's outcome may be unknown; no automatic retry" : ""), null);
        } finally {
            // A task already running must finish before returning this socket to the idle pool.
            synchronized (deadlineLock) { pending[0] = false; if (expiry != null) expiry.cancel(false); }
            if (socket != null) release(socket, reusable);
        }
    }

    private Object read(Socket socket, long deadline) throws IOException {
        ByteArrayOutputStream frame = new ByteArrayOutputStream(Math.min(8192, options.getMaxResponseBytes()));
        byte[] chunk = new byte[8192];
        InputStream input = socket.getInputStream();
        while (true) {
            socket.setSoTimeout(remaining(deadline));
            int count = input.read(chunk);
            if (count < 0) throw new PacificDBException("connection_lost");
            for (int i = 0; i < count; i++) {
                if (chunk[i] == '\n') {
                    if (i != count - 1 || input.available() > 0) throw new PacificDBException("invalid_response");
                    try {
                        String text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(frame.toByteArray())).toString();
                        Object value = JSON.readValue(text, Object.class);
                        if (!(value instanceof Map) && !(value instanceof List)) throw new PacificDBException("invalid_response");
                        return value;
                    } catch (IOException error) { throw new PacificDBException("invalid_response"); }
                }
                if (frame.size() >= options.getMaxResponseBytes()) throw new PacificDBException("response_too_large");
                frame.write(chunk[i]);
            }
        }
    }

    @Override public synchronized void close() {
        if (closed) return;
        closed = true;
        sockets.forEach(ConnectionPool::closeSocket); sockets.clear(); idle.clear();
        timer.shutdownNow(); notifyAll();
    }
}
