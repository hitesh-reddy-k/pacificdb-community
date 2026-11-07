from .client import PacificDBClient
from .connection import parse_connection_url
from .errors import PacificDBError, MediaUploadError

PacificDB = PacificDBClient
__all__ = ["PacificDB", "PacificDBClient", "PacificDBError", "MediaUploadError", "parse_connection_url"]

__version__ = '1.2.0rc2'
release_info = {'version': '1.2.0', 'releaseVersion': '1.2.0-rc.2', 'name': 'Pre-release v1.2', 'channel': 'prerelease', 'tag': 'v1.2.0-rc.2'}
