"""Public engine/transport errors; diagnostic values contain no known secrets."""
import re


def sanitize(value, secrets=()):
    if isinstance(value, str):
        value = re.sub(r'pacificdbs?://[^\s/]*@', 'pacificdb://[redacted]@', value)
        for secret in secrets:
            if secret:
                value = value.replace(secret, '[redacted]')
        return value
    if isinstance(value, dict):
        return {sanitize(key, secrets): '[redacted]' if re.search('password|token|authorization', str(key), re.I)
                else sanitize(item, secrets) for key, item in value.items()}
    if isinstance(value, list):
        return [sanitize(item, secrets) for item in value]
    return value


class PacificDBError(RuntimeError):
    def __init__(self, code, message=None, response=None):
        self.code = str(code)
        self.response = response
        super().__init__(message or self.code)


class MediaUploadError(PacificDBError):
    def __init__(self, code, message=None, response=None, *, upload_id=None,
                 next_chunk=0, received_chunks=0, received_bytes=0, resumable=False):
        super().__init__(code, message, response)
        self.upload_id, self.next_chunk = upload_id, next_chunk
        self.received_chunks, self.received_bytes = received_chunks, received_bytes
        self.resumable = resumable
