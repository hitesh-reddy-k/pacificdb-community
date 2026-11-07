from .client import PacificDBClient
from .connection import parse_connection_url
from .errors import PacificDBError, MediaUploadError

PacificDB = PacificDBClient
__all__ = ["PacificDB", "PacificDBClient", "PacificDBError", "MediaUploadError", "parse_connection_url"]

__version__ = '1.2.0'
release_info = {'version': '1.2.0', 'releaseVersion': '1.2.0', 'name': 'PacificDB 1.2.0', 'channel': 'stable', 'tag': 'v1.2.0'}
