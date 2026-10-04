"""Named advanced engine operations. Field names remain the engine's JSON keys."""
from .errors import PacificDBError


class _Family:
    def __init__(self, client): self._client = client

    def _call(self, action, options, scope='principal'):
        if {'action', 'userId', 'dbName', 'db', 'token'} & options.keys():
            raise PacificDBError('invalid_request', 'Options cannot replace action or captured client scope')
        command = {'action': action, **options}
        if scope == 'collection':
            self._client._name(options.get('collection'), 'collection')
        return self._client._database_request(command) if scope in ('database','collection') else self._client.request(command)

    def _token_result(self, result):
        if not isinstance(result, dict) or not isinstance(result.get('token'), str) or not result['token']:
            raise PacificDBError('invalid_response', 'Invalid token response')
        self._client.token = result['token']
        return result


class Indexes(_Family):
    def create(self, **options):
        """Send createIndex; return the original decoded engine response."""
        return self._call("createIndex", options, "collection")

    def list(self, **options):
        """Send listIndexes; return the original decoded engine response."""
        return self._call("listIndexes", options, "collection")

    def validate(self, **options):
        """Send validateIndex; return the original decoded engine response."""
        return self._call("validateIndex", options, "collection")

    def rebuild(self, **options):
        """Send rebuildIndex; return the original decoded engine response."""
        return self._call("rebuildIndex", options, "collection")

    def drop(self, **options):
        """Send dropIndex; return the original decoded engine response."""
        return self._call("dropIndex", options, "collection")



class Vectors(_Family):
    def insert(self, **options):
        """Send insertVector; return the original decoded engine response."""
        return self._call("insertVector", options, "collection")

    def query(self, **options):
        """Send queryVector; return the original decoded engine response."""
        return self._call("queryVector", options, "collection")



class Media(_Family):
    def put(self, collection, media_id, data, metadata=None):
        from .files import put
        return put(self._client, collection, media_id, data, metadata)

    def get(self, collection, media_id):
        from .files import get
        return get(self._client, collection, media_id)

    def upload_file(self, collection, path, *, content_type=None, chunk_bytes=None, resume=None):
        from .files import upload_file
        return upload_file(self._client, collection, path, content_type=content_type, chunk_bytes=chunk_bytes, resume=resume)

    def download_file(self, media_id, destination, *, collection=None):
        from .files import download_file
        return download_file(self._client, media_id, destination, collection=collection)

    def begin(self, **options):
        """Send community_media_begin; return the original decoded engine response."""
        return self._call("community_media_begin", options, "database")

    def put_chunk(self, **options):
        """Send community_media_put_chunk; return the original decoded engine response."""
        return self._call("community_media_put_chunk", options, "principal")

    def finalize_upload(self, **options):
        """Send community_media_finalize; return the original decoded engine response."""
        return self._call("community_media_finalize", options, "principal")

    def list(self, **options):
        """Send community_media_list; return the original decoded engine response."""
        return self._call("community_media_list", options, "database")

    def get_manifest(self, **options):
        """Send community_media_get; return the original decoded engine response."""
        return self._call("community_media_get", options, "principal")

    def get_chunk(self, **options):
        """Send community_media_get_chunk; return the original decoded engine response."""
        return self._call("community_media_get_chunk", options, "principal")

    def delete(self, **options):
        """Send community_media_delete; return the original decoded engine response."""
        return self._call("community_media_delete", options, "principal")

    def cleanup(self, **options):
        """Send community_media_cleanup; return the original decoded engine response."""
        return self._call("community_media_cleanup", options, "principal")



class Backups(_Family):
    def export(self, backup_id, destination, *, chunk_bytes=1024*1024):
        from .files import export_backup
        return export_backup(self._client, backup_id, destination, chunk_bytes=chunk_bytes)

    def create(self, **options):
        """Send create_backup; return the original decoded engine response."""
        return self._call("create_backup", options, "global")

    def list(self, **options):
        """Send list_backups; return the original decoded engine response."""
        return self._call("list_backups", options, "principal")

    def get(self, **options):
        """Send get_backup; return the original decoded engine response."""
        return self._call("get_backup", options, "principal")

    def export_manifest(self, **options):
        """Send export_backup_manifest; return the original decoded engine response."""
        return self._call("export_backup_manifest", options, "principal")

    def export_file_chunk(self, **options):
        """Send export_backup_file_chunk; return the original decoded engine response."""
        return self._call("export_backup_file_chunk", options, "principal")

    def delete(self, **options):
        """Send delete_backup; return the original decoded engine response."""
        return self._call("delete_backup", options, "global")

    def list_restores(self, **options):
        """Send list_restores; return the original decoded engine response."""
        return self._call("list_restores", options, "principal")

    def verify(self, **options):
        """Send verify_backup; return the original decoded engine response."""
        return self._call("verify_backup", options, "principal")

    def restore(self, **options):
        """Send restore_backup; return the original decoded engine response."""
        return self._call("restore_backup", options, "global")



class Security(_Family):
    def use_token(self, token):
        if not isinstance(token, str) or not token: raise PacificDBError("invalid_token")
        with self._client._lock:
            if self._client._closed: raise PacificDBError("client_closed")
            self._client.token = token

    def authenticate(self, **options):
        """Send security_authenticate; return the original decoded engine response."""
        return self._token_result(self._call("security_authenticate", options, "principal"))

    def validate_token(self, **options):
        """Send security_validate_token; return the original decoded engine response."""
        return self._call("security_validate_token", options, "principal")

    def whoami(self, **options):
        """Send security_whoami; return the original decoded engine response."""
        return self._call("security_whoami", options, "principal")

    def create_api_key(self, **options):
        """Send api_key_create; return the original decoded engine response."""
        return self._call("api_key_create", options, "principal")

    def list_api_keys(self, **options):
        """Send api_key_list; return the original decoded engine response."""
        return self._call("api_key_list", options, "principal")

    def get_api_key(self, **options):
        """Send api_key_get; return the original decoded engine response."""
        return self._call("api_key_get", options, "principal")

    def revoke_api_key(self, **options):
        """Send api_key_revoke; return the original decoded engine response."""
        return self._call("api_key_revoke", options, "principal")

    def refresh_token(self, **options):
        """Send security_refresh_token; return the original decoded engine response."""
        return self._token_result(self._call("security_refresh_token", options, "principal"))

    def metrics(self, **options):
        """Send security_metrics; return the original decoded engine response."""
        return self._call("security_metrics", options, "principal")



class Admin(_Family):
    def ping(self, **options):
        """Send ping; return the original decoded engine response."""
        return self._call("ping", options, "principal")

    def community_project_create(self, **options):
        """Send community_project_create; return the original decoded engine response."""
        return self._call("community_project_create", options, "principal")

    def community_project_list(self, **options):
        """Send community_project_list; return the original decoded engine response."""
        return self._call("community_project_list", options, "principal")

    def community_project_get(self, **options):
        """Send community_project_get; return the original decoded engine response."""
        return self._call("community_project_get", options, "principal")

    def community_project_delete(self, **options):
        """Send community_project_delete; return the original decoded engine response."""
        return self._call("community_project_delete", options, "principal")

    def community_database_map(self, **options):
        """Send community_database_map; return the original decoded engine response."""
        return self._call("community_database_map", options, "principal")

    def community_database_list(self, **options):
        """Send community_database_list; return the original decoded engine response."""
        return self._call("community_database_list", options, "principal")

    def community_database_project(self, **options):
        """Send community_database_project; return the original decoded engine response."""
        return self._call("community_database_project", options, "principal")

    def observe_leader_term(self, **options):
        """Send observeLeaderTerm; return the original decoded engine response."""
        return self._call("observeLeaderTerm", options, "principal")

    def init_user_space(self, **options):
        """Send initUserSpace; return the original decoded engine response."""
        return self._call("initUserSpace", options, "principal")

    def op_status(self, **options):
        """Send opStatus; return the original decoded engine response."""
        return self._call("opStatus", options, "principal")

    def get_metrics(self, **options):
        """Send get_metrics; return the original decoded engine response."""
        return self._call("get_metrics", options, "principal")

    def health_check(self, **options):
        """Send health_check; return the original decoded engine response."""
        return self._call("health_check", options, "principal")

    def get_cluster_status(self, **options):
        """Send get_cluster_status; return the original decoded engine response."""
        return self._call("get_cluster_status", options, "principal")

    def split_shard(self, **options):
        """Send split_shard; return the original decoded engine response."""
        return self._call("split_shard", options, "global")

    def get_shard_details(self, **options):
        """Send get_shard_details; return the original decoded engine response."""
        return self._call("get_shard_details", options, "principal")

    def list_shards(self, **options):
        """Send list_shards; return the original decoded engine response."""
        return self._call("list_shards", options, "principal")

    def config_get(self, **options):
        """Send config_get; return the original decoded engine response."""
        return self._call("config_get", options, "global")

    def config_set(self, **options):
        """Send config_set; return the original decoded engine response."""
        return self._call("config_set", options, "global")

    def config_reload(self, **options):
        """Send config_reload; return the original decoded engine response."""
        return self._call("config_reload", options, "global")

    def config_dump(self, **options):
        """Send config_dump; return the original decoded engine response."""
        return self._call("config_dump", options, "global")

    def raft_status(self, **options):
        """Send admin_raft_status; return the original decoded engine response."""
        return self._call("admin_raft_status", options, "principal")

    def replica_digest(self, **options):
        """Send admin_replica_digest; return the original decoded engine response."""
        return self._call("admin_replica_digest", options, "collection")

    def apply_status(self, **options):
        """Send admin_apply_status; return the original decoded engine response."""
        return self._call("admin_apply_status", options, "principal")

    def logical_write_status(self, **options):
        """Send admin_logical_write_status; return the original decoded engine response."""
        return self._call("admin_logical_write_status", options, "collection")

    def storage_visibility_check(self, **options):
        """Send admin_storage_visibility_check; return the original decoded engine response."""
        return self._call("admin_storage_visibility_check", options, "collection")

    def dashboard(self, **options):
        """Send admin_dashboard; return the original decoded engine response."""
        return self._call("admin_dashboard", options, "principal")

    def create_tenant(self, **options):
        """Send createTenant; return the original decoded engine response."""
        return self._call("createTenant", options, "principal")

    def delete_tenant(self, **options):
        """Send deleteTenant; return the original decoded engine response."""
        return self._call("deleteTenant", options, "principal")

    def list_tenants(self, **options):
        """Send listTenants; return the original decoded engine response."""
        return self._call("listTenants", options, "principal")

    def get_tenant(self, **options):
        """Send getTenant; return the original decoded engine response."""
        return self._call("getTenant", options, "principal")

    def update_tenant(self, **options):
        """Send updateTenant; return the original decoded engine response."""
        return self._call("updateTenant", options, "principal")

    def create_tenant_user(self, **options):
        """Send createTenantUser; return the original decoded engine response."""
        return self._call("createTenantUser", options, "principal")

    def delete_tenant_user(self, **options):
        """Send deleteTenantUser; return the original decoded engine response."""
        return self._call("deleteTenantUser", options, "principal")

    def update_tenant_user_role(self, **options):
        """Send updateTenantUserRole; return the original decoded engine response."""
        return self._call("updateTenantUserRole", options, "principal")

    def update_tenant_user_password(self, **options):
        """Send updateTenantUserPassword; return the original decoded engine response."""
        return self._call("updateTenantUserPassword", options, "principal")

    def set_tenant_user_database_access(self, **options):
        """Send setTenantUserDatabaseAccess; return the original decoded engine response."""
        return self._call("setTenantUserDatabaseAccess", options, "principal")

    def list_tenant_users(self, **options):
        """Send listTenantUsers; return the original decoded engine response."""
        return self._call("listTenantUsers", options, "principal")

    def tenant_authenticate(self, **options):
        """Send tenantAuthenticate; return the original decoded engine response."""
        return self._call("tenantAuthenticate", options, "principal")

    def tenant_validate_session(self, **options):
        """Send tenantValidateSession; return the original decoded engine response."""
        return self._call("tenantValidateSession", options, "principal")

    def tenant_revoke_session(self, **options):
        """Send tenantRevokeSession; return the original decoded engine response."""
        return self._call("tenantRevokeSession", options, "principal")

    def tenant_check_access(self, **options):
        """Send tenantCheckAccess; return the original decoded engine response."""
        return self._call("tenantCheckAccess", options, "principal")

    def tenant_get_stats(self, **options):
        """Send tenantGetStats; return the original decoded engine response."""
        return self._call("tenantGetStats", options, "principal")

    def tenant_get_audit_log(self, **options):
        """Send tenantGetAuditLog; return the original decoded engine response."""
        return self._call("tenantGetAuditLog", options, "principal")

    def tenant_create_database(self, **options):
        """Send tenantCreateDatabase; return the original decoded engine response."""
        return self._call("tenantCreateDatabase", options, "database")

    def tenant_drop_database(self, **options):
        """Send tenantDropDatabase; return the original decoded engine response."""
        return self._call("tenantDropDatabase", options, "database")

    def tenant_list_databases(self, **options):
        """Send tenantListDatabases; return the original decoded engine response."""
        return self._call("tenantListDatabases", options, "principal")

    def tenant_create_collection(self, **options):
        """Send tenantCreateCollection; return the original decoded engine response."""
        return self._call("tenantCreateCollection", options, "database")

    def tenant_drop_collection(self, **options):
        """Send tenantDropCollection; return the original decoded engine response."""
        return self._call("tenantDropCollection", options, "database")

    def register_node(self, **options):
        """Send register_node; return the original decoded engine response."""
        return self._call("register_node", options, "global")

    def deregister_node(self, **options):
        """Send deregister_node; return the original decoded engine response."""
        return self._call("deregister_node", options, "global")

    def create_shard(self, **options):
        """Send create_shard; return the original decoded engine response."""
        return self._call("create_shard", options, "global")

    def migrate_shard(self, **options):
        """Send migrate_shard; return the original decoded engine response."""
        return self._call("migrate_shard", options, "global")

    def rebalance_shards(self, **options):
        """Send rebalance_shards; return the original decoded engine response."""
        return self._call("rebalance_shards", options, "global")

    def cluster_route(self, **options):
        """Send cluster_route; return the original decoded engine response."""
        return self._call("cluster_route", options, "principal")

    def storage_stats(self, **options):
        """Send storage_stats; return the original decoded engine response."""
        return self._call("storage_stats", options, "global")

    def wal_status(self, **options):
        """Send wal_status; return the original decoded engine response."""
        return self._call("wal_status", options, "global")

    def compaction_status(self, **options):
        """Send admin_compaction_status; return the original decoded engine response."""
        return self._call("admin_compaction_status", options, "global")

    def replay_check(self, **options):
        """Send admin_replay_check; return the original decoded engine response."""
        return self._call("admin_replay_check", options, "global")

    def memtable_status(self, **options):
        """Send memtable_status; return the original decoded engine response."""
        return self._call("memtable_status", options, "global")

    def sst_status(self, **options):
        """Send sst_status; return the original decoded engine response."""
        return self._call("sst_status", options, "global")

    def storage_flush(self, **options):
        """Send storage_flush; return the original decoded engine response."""
        return self._call("storage_flush", options, "global")

    def storage_compact(self, **options):
        """Send storage_compact; return the original decoded engine response."""
        return self._call("storage_compact", options, "global")

    def storage_repair(self, **options):
        """Send storage_repair; return the original decoded engine response."""
        return self._call("storage_repair", options, "global")

    def verify_integrity(self, **options):
        """Send verify_integrity; return the original decoded engine response."""
        return self._call("verify_integrity", options, "principal")

    def lsm_metrics(self, **options):
        """Send lsm_metrics; return the original decoded engine response."""
        return self._call("lsm_metrics", options, "global")

