package io.pacificdb;

/** Upload interruption with explicit progress for caller-directed resume. */
public final class MediaUploadException extends PacificDBException {
    private final String uploadId;
    private final long nextChunk, receivedChunks, receivedBytes;
    private final boolean resumable;

    public MediaUploadException(String code, String message, Object response, String uploadId,
                                long nextChunk, long receivedChunks, long receivedBytes, boolean resumable) {
        super(code, message, response);
        this.uploadId = uploadId;
        this.nextChunk = nextChunk;
        this.receivedChunks = receivedChunks;
        this.receivedBytes = receivedBytes;
        this.resumable = resumable;
    }
    public String getUploadId() { return uploadId; }
    public long getNextChunk() { return nextChunk; }
    public long getReceivedChunks() { return receivedChunks; }
    public long getReceivedBytes() { return receivedBytes; }
    public boolean isResumable() { return resumable; }
}
