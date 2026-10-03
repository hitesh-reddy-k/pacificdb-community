package io.pacificdb;

/** Engine or transport failure. Sent writes are never retried automatically. */
public class PacificDBException extends IllegalStateException {
    private final String code;
    private final Object response;

    public PacificDBException(String code, String message, Object response) {
        super(message == null ? code : message);
        this.code = code;
        this.response = response;
    }

    public PacificDBException(String code) { this(code, code, null); }
    public String getCode() { return code; }
    public Object getResponse() { return response; }
}
