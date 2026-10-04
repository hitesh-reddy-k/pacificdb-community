package io.pacificdb;

import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

/** Named advanced engine operations. Options use exact engine JSON field names. */
public final class Operations {
    private Operations() { }
    private static class Family {
        final PacificDBClient client;
        Family(PacificDBClient client) { this.client = client; }
        Object call(String action, Map<String, Object> options, String scope) {
            if (options == null || options.keySet().stream().anyMatch(Set.of("action","userId","dbName","db","token")::contains))
                throw new PacificDBException("invalid_request", "Options cannot replace action or captured client scope", null);
            Map<String, Object> command = new LinkedHashMap<>(options); command.put("action", action);
            if (scope.equals("collection")) {
                Object collection = options.get("collection");
                if (!(collection instanceof String)) throw new PacificDBException("invalid_collection");
                return client.collectionRequest((String)collection, action, options);
            }
            return scope.equals("database") ? client.databaseRequest(command) : client.requestValue(command);
        }
        Object tokenResult(Object result) {
            Object token = result instanceof Map ? ((Map<?, ?>)result).get("token") : null;
            if (!(token instanceof String) || ((String)token).isEmpty()) throw new PacificDBException("invalid_response", "Invalid token response", null);
            client.useToken((String)token);
            return result;
        }
    }

    public static final class Indexes extends Family {
        Indexes(PacificDBClient client) { super(client); }
        /** Send createIndex; fields and response retain their engine contracts. */
        public Object create(Map<String, Object> options) { return call("createIndex", options, "collection"); }
        public Object create() { return create(Map.of()); }
        /** Send listIndexes; fields and response retain their engine contracts. */
        public Object list(Map<String, Object> options) { return call("listIndexes", options, "collection"); }
        public Object list() { return list(Map.of()); }
        /** Send validateIndex; fields and response retain their engine contracts. */
        public Object validate(Map<String, Object> options) { return call("validateIndex", options, "collection"); }
        public Object validate() { return validate(Map.of()); }
        /** Send rebuildIndex; fields and response retain their engine contracts. */
        public Object rebuild(Map<String, Object> options) { return call("rebuildIndex", options, "collection"); }
        public Object rebuild() { return rebuild(Map.of()); }
        /** Send dropIndex; fields and response retain their engine contracts. */
        public Object drop(Map<String, Object> options) { return call("dropIndex", options, "collection"); }
        public Object drop() { return drop(Map.of()); }
    }

    public static final class Vectors extends Family {
        Vectors(PacificDBClient client) { super(client); }
        /** Send insertVector; fields and response retain their engine contracts. */
        public Object insert(Map<String, Object> options) { return call("insertVector", options, "collection"); }
        public Object insert() { return insert(Map.of()); }
        /** Send queryVector; fields and response retain their engine contracts. */
        public Object query(Map<String, Object> options) { return call("queryVector", options, "collection"); }
        public Object query() { return query(Map.of()); }
    }

    public static final class Media extends Family {
        Media(PacificDBClient client) { super(client); }
        public Object put(String collection, String id, byte[] data) { return put(collection,id,data,Map.of()); }
        public Object put(String collection, String id, byte[] data, Map<String,Object> metadata) { return FileTransfers.put(client,collection,id,data,metadata); }
        public Map<String,Object> get(String collection, String id) { return FileTransfers.get(client,collection,id); }
        public Map<String,Object> uploadFile(String collection, Path path) { return uploadFile(collection,path,null,null,null); }
        public Map<String,Object> uploadFile(String collection, Path path, String contentType, Integer chunkBytes, String resumeId) { return FileTransfers.upload(client,collection,path,contentType,chunkBytes,resumeId); }
        public Map<String,Object> downloadFile(String id, Path destination) { return downloadFile(id,destination,null); }
        public Map<String,Object> downloadFile(String id, Path destination, String collection) { return FileTransfers.download(client,id,destination,collection); }
        /** Send community_media_begin; fields and response retain their engine contracts. */
        public Object begin(Map<String, Object> options) { return call("community_media_begin", options, "database"); }
        public Object begin() { return begin(Map.of()); }
        /** Send community_media_put_chunk; fields and response retain their engine contracts. */
        public Object putChunk(Map<String, Object> options) { return call("community_media_put_chunk", options, "principal"); }
        public Object putChunk() { return putChunk(Map.of()); }
        /** Send community_media_finalize; fields and response retain their engine contracts. */
        public Object finalizeUpload(Map<String, Object> options) { return call("community_media_finalize", options, "principal"); }
        public Object finalizeUpload() { return finalizeUpload(Map.of()); }
        /** Send community_media_list; fields and response retain their engine contracts. */
        public Object list(Map<String, Object> options) { return call("community_media_list", options, "database"); }
        public Object list() { return list(Map.of()); }
        /** Send community_media_get; fields and response retain their engine contracts. */
        public Object getManifest(Map<String, Object> options) { return call("community_media_get", options, "principal"); }
        public Object getManifest() { return getManifest(Map.of()); }
        /** Send community_media_get_chunk; fields and response retain their engine contracts. */
        public Object getChunk(Map<String, Object> options) { return call("community_media_get_chunk", options, "principal"); }
        public Object getChunk() { return getChunk(Map.of()); }
        /** Send community_media_delete; fields and response retain their engine contracts. */
        public Object delete(Map<String, Object> options) { return call("community_media_delete", options, "principal"); }
        public Object delete() { return delete(Map.of()); }
        /** Send community_media_cleanup; fields and response retain their engine contracts. */
        public Object cleanup(Map<String, Object> options) { return call("community_media_cleanup", options, "principal"); }
        public Object cleanup() { return cleanup(Map.of()); }
    }

    public static final class Backups extends Family {
        Backups(PacificDBClient client) { super(client); }
        public Map<String,Object> export(String id, Path destination) { return export(id,destination,1048576); }
        public Map<String,Object> export(String id, Path destination, int chunkBytes) { return FileTransfers.export(client,id,destination,chunkBytes); }
        /** Send create_backup; fields and response retain their engine contracts. */
        public Object create(Map<String, Object> options) { return call("create_backup", options, "global"); }
        public Object create() { return create(Map.of()); }
        /** Send list_backups; fields and response retain their engine contracts. */
        public Object list(Map<String, Object> options) { return call("list_backups", options, "principal"); }
        public Object list() { return list(Map.of()); }
        /** Send get_backup; fields and response retain their engine contracts. */
        public Object get(Map<String, Object> options) { return call("get_backup", options, "principal"); }
        public Object get() { return get(Map.of()); }
        /** Send export_backup_manifest; fields and response retain their engine contracts. */
        public Object exportManifest(Map<String, Object> options) { return call("export_backup_manifest", options, "principal"); }
        public Object exportManifest() { return exportManifest(Map.of()); }
        /** Send export_backup_file_chunk; fields and response retain their engine contracts. */
        public Object exportFileChunk(Map<String, Object> options) { return call("export_backup_file_chunk", options, "principal"); }
        public Object exportFileChunk() { return exportFileChunk(Map.of()); }
        /** Send delete_backup; fields and response retain their engine contracts. */
        public Object delete(Map<String, Object> options) { return call("delete_backup", options, "global"); }
        public Object delete() { return delete(Map.of()); }
        /** Send list_restores; fields and response retain their engine contracts. */
        public Object listRestores(Map<String, Object> options) { return call("list_restores", options, "principal"); }
        public Object listRestores() { return listRestores(Map.of()); }
        /** Send verify_backup; fields and response retain their engine contracts. */
        public Object verify(Map<String, Object> options) { return call("verify_backup", options, "principal"); }
        public Object verify() { return verify(Map.of()); }
        /** Send restore_backup; fields and response retain their engine contracts. */
        public Object restore(Map<String, Object> options) { return call("restore_backup", options, "global"); }
        public Object restore() { return restore(Map.of()); }
    }

    public static final class Security extends Family {
        Security(PacificDBClient client) { super(client); }
        public void useToken(String token) { client.useToken(token); }
        /** Send security_authenticate; fields and response retain their engine contracts. */
        public Object authenticate(Map<String, Object> options) { return tokenResult(call("security_authenticate", options, "principal")); }
        public Object authenticate() { return authenticate(Map.of()); }
        /** Send security_validate_token; fields and response retain their engine contracts. */
        public Object validateToken(Map<String, Object> options) { return call("security_validate_token", options, "principal"); }
        public Object validateToken() { return validateToken(Map.of()); }
        /** Send security_whoami; fields and response retain their engine contracts. */
        public Object whoami(Map<String, Object> options) { return call("security_whoami", options, "principal"); }
        public Object whoami() { return whoami(Map.of()); }
        /** Send api_key_create; fields and response retain their engine contracts. */
        public Object createApiKey(Map<String, Object> options) { return call("api_key_create", options, "principal"); }
        public Object createApiKey() { return createApiKey(Map.of()); }
        /** Send api_key_list; fields and response retain their engine contracts. */
        public Object listApiKeys(Map<String, Object> options) { return call("api_key_list", options, "principal"); }
        public Object listApiKeys() { return listApiKeys(Map.of()); }
        /** Send api_key_get; fields and response retain their engine contracts. */
        public Object getApiKey(Map<String, Object> options) { return call("api_key_get", options, "principal"); }
        public Object getApiKey() { return getApiKey(Map.of()); }
        /** Send api_key_revoke; fields and response retain their engine contracts. */
        public Object revokeApiKey(Map<String, Object> options) { return call("api_key_revoke", options, "principal"); }
        public Object revokeApiKey() { return revokeApiKey(Map.of()); }
        /** Send security_refresh_token; fields and response retain their engine contracts. */
        public Object refreshToken(Map<String, Object> options) { return tokenResult(call("security_refresh_token", options, "principal")); }
        public Object refreshToken() { return refreshToken(Map.of()); }
        /** Send security_metrics; fields and response retain their engine contracts. */
        public Object metrics(Map<String, Object> options) { return call("security_metrics", options, "principal"); }
        public Object metrics() { return metrics(Map.of()); }
    }

    public static final class Admin extends Family {
        Admin(PacificDBClient client) { super(client); }
        /** Send ping; fields and response retain their engine contracts. */
        public Object ping(Map<String, Object> options) { return call("ping", options, "principal"); }
        public Object ping() { return ping(Map.of()); }
        /** Send community_project_create; fields and response retain their engine contracts. */
        public Object communityProjectCreate(Map<String, Object> options) { return call("community_project_create", options, "principal"); }
        public Object communityProjectCreate() { return communityProjectCreate(Map.of()); }
        /** Send community_project_list; fields and response retain their engine contracts. */
        public Object communityProjectList(Map<String, Object> options) { return call("community_project_list", options, "principal"); }
        public Object communityProjectList() { return communityProjectList(Map.of()); }
        /** Send community_project_get; fields and response retain their engine contracts. */
        public Object communityProjectGet(Map<String, Object> options) { return call("community_project_get", options, "principal"); }
        public Object communityProjectGet() { return communityProjectGet(Map.of()); }
        /** Send community_project_delete; fields and response retain their engine contracts. */
        public Object communityProjectDelete(Map<String, Object> options) { return call("community_project_delete", options, "principal"); }
        public Object communityProjectDelete() { return communityProjectDelete(Map.of()); }
        /** Send community_database_map; fields and response retain their engine contracts. */
        public Object communityDatabaseMap(Map<String, Object> options) { return call("community_database_map", options, "principal"); }
        public Object communityDatabaseMap() { return communityDatabaseMap(Map.of()); }
        /** Send community_database_list; fields and response retain their engine contracts. */
        public Object communityDatabaseList(Map<String, Object> options) { return call("community_database_list", options, "principal"); }
        public Object communityDatabaseList() { return communityDatabaseList(Map.of()); }
        /** Send community_database_project; fields and response retain their engine contracts. */
        public Object communityDatabaseProject(Map<String, Object> options) { return call("community_database_project", options, "principal"); }
        public Object communityDatabaseProject() { return communityDatabaseProject(Map.of()); }
        /** Send observeLeaderTerm; fields and response retain their engine contracts. */
        public Object observeLeaderTerm(Map<String, Object> options) { return call("observeLeaderTerm", options, "principal"); }
        public Object observeLeaderTerm() { return observeLeaderTerm(Map.of()); }
        /** Send initUserSpace; fields and response retain their engine contracts. */
        public Object initUserSpace(Map<String, Object> options) { return call("initUserSpace", options, "principal"); }
        public Object initUserSpace() { return initUserSpace(Map.of()); }
        /** Send opStatus; fields and response retain their engine contracts. */
        public Object opStatus(Map<String, Object> options) { return call("opStatus", options, "principal"); }
        public Object opStatus() { return opStatus(Map.of()); }
        /** Send get_metrics; fields and response retain their engine contracts. */
        public Object getMetrics(Map<String, Object> options) { return call("get_metrics", options, "principal"); }
        public Object getMetrics() { return getMetrics(Map.of()); }
        /** Send health_check; fields and response retain their engine contracts. */
        public Object healthCheck(Map<String, Object> options) { return call("health_check", options, "principal"); }
        public Object healthCheck() { return healthCheck(Map.of()); }
        /** Send get_cluster_status; fields and response retain their engine contracts. */
        public Object getClusterStatus(Map<String, Object> options) { return call("get_cluster_status", options, "principal"); }
        public Object getClusterStatus() { return getClusterStatus(Map.of()); }
        /** Send split_shard; fields and response retain their engine contracts. */
        public Object splitShard(Map<String, Object> options) { return call("split_shard", options, "global"); }
        public Object splitShard() { return splitShard(Map.of()); }
        /** Send get_shard_details; fields and response retain their engine contracts. */
        public Object getShardDetails(Map<String, Object> options) { return call("get_shard_details", options, "principal"); }
        public Object getShardDetails() { return getShardDetails(Map.of()); }
        /** Send list_shards; fields and response retain their engine contracts. */
        public Object listShards(Map<String, Object> options) { return call("list_shards", options, "principal"); }
        public Object listShards() { return listShards(Map.of()); }
        /** Send config_get; fields and response retain their engine contracts. */
        public Object configGet(Map<String, Object> options) { return call("config_get", options, "global"); }
        public Object configGet() { return configGet(Map.of()); }
        /** Send config_set; fields and response retain their engine contracts. */
        public Object configSet(Map<String, Object> options) { return call("config_set", options, "global"); }
        public Object configSet() { return configSet(Map.of()); }
        /** Send config_reload; fields and response retain their engine contracts. */
        public Object configReload(Map<String, Object> options) { return call("config_reload", options, "global"); }
        public Object configReload() { return configReload(Map.of()); }
        /** Send config_dump; fields and response retain their engine contracts. */
        public Object configDump(Map<String, Object> options) { return call("config_dump", options, "global"); }
        public Object configDump() { return configDump(Map.of()); }
        /** Send admin_raft_status; fields and response retain their engine contracts. */
        public Object raftStatus(Map<String, Object> options) { return call("admin_raft_status", options, "principal"); }
        public Object raftStatus() { return raftStatus(Map.of()); }
        /** Send admin_replica_digest; fields and response retain their engine contracts. */
        public Object replicaDigest(Map<String, Object> options) { return call("admin_replica_digest", options, "collection"); }
        public Object replicaDigest() { return replicaDigest(Map.of()); }
        /** Send admin_apply_status; fields and response retain their engine contracts. */
        public Object applyStatus(Map<String, Object> options) { return call("admin_apply_status", options, "principal"); }
        public Object applyStatus() { return applyStatus(Map.of()); }
        /** Send admin_logical_write_status; fields and response retain their engine contracts. */
        public Object logicalWriteStatus(Map<String, Object> options) { return call("admin_logical_write_status", options, "collection"); }
        public Object logicalWriteStatus() { return logicalWriteStatus(Map.of()); }
        /** Send admin_storage_visibility_check; fields and response retain their engine contracts. */
        public Object storageVisibilityCheck(Map<String, Object> options) { return call("admin_storage_visibility_check", options, "collection"); }
        public Object storageVisibilityCheck() { return storageVisibilityCheck(Map.of()); }
        /** Send admin_dashboard; fields and response retain their engine contracts. */
        public Object dashboard(Map<String, Object> options) { return call("admin_dashboard", options, "principal"); }
        public Object dashboard() { return dashboard(Map.of()); }
        /** Send createTenant; fields and response retain their engine contracts. */
        public Object createTenant(Map<String, Object> options) { return call("createTenant", options, "principal"); }
        public Object createTenant() { return createTenant(Map.of()); }
        /** Send deleteTenant; fields and response retain their engine contracts. */
        public Object deleteTenant(Map<String, Object> options) { return call("deleteTenant", options, "principal"); }
        public Object deleteTenant() { return deleteTenant(Map.of()); }
        /** Send listTenants; fields and response retain their engine contracts. */
        public Object listTenants(Map<String, Object> options) { return call("listTenants", options, "principal"); }
        public Object listTenants() { return listTenants(Map.of()); }
        /** Send getTenant; fields and response retain their engine contracts. */
        public Object getTenant(Map<String, Object> options) { return call("getTenant", options, "principal"); }
        public Object getTenant() { return getTenant(Map.of()); }
        /** Send updateTenant; fields and response retain their engine contracts. */
        public Object updateTenant(Map<String, Object> options) { return call("updateTenant", options, "principal"); }
        public Object updateTenant() { return updateTenant(Map.of()); }
        /** Send createTenantUser; fields and response retain their engine contracts. */
        public Object createTenantUser(Map<String, Object> options) { return call("createTenantUser", options, "principal"); }
        public Object createTenantUser() { return createTenantUser(Map.of()); }
        /** Send deleteTenantUser; fields and response retain their engine contracts. */
        public Object deleteTenantUser(Map<String, Object> options) { return call("deleteTenantUser", options, "principal"); }
        public Object deleteTenantUser() { return deleteTenantUser(Map.of()); }
        /** Send updateTenantUserRole; fields and response retain their engine contracts. */
        public Object updateTenantUserRole(Map<String, Object> options) { return call("updateTenantUserRole", options, "principal"); }
        public Object updateTenantUserRole() { return updateTenantUserRole(Map.of()); }
        /** Send updateTenantUserPassword; fields and response retain their engine contracts. */
        public Object updateTenantUserPassword(Map<String, Object> options) { return call("updateTenantUserPassword", options, "principal"); }
        public Object updateTenantUserPassword() { return updateTenantUserPassword(Map.of()); }
        /** Send setTenantUserDatabaseAccess; fields and response retain their engine contracts. */
        public Object setTenantUserDatabaseAccess(Map<String, Object> options) { return call("setTenantUserDatabaseAccess", options, "principal"); }
        public Object setTenantUserDatabaseAccess() { return setTenantUserDatabaseAccess(Map.of()); }
        /** Send listTenantUsers; fields and response retain their engine contracts. */
        public Object listTenantUsers(Map<String, Object> options) { return call("listTenantUsers", options, "principal"); }
        public Object listTenantUsers() { return listTenantUsers(Map.of()); }
        /** Send tenantAuthenticate; fields and response retain their engine contracts. */
        public Object tenantAuthenticate(Map<String, Object> options) { return call("tenantAuthenticate", options, "principal"); }
        public Object tenantAuthenticate() { return tenantAuthenticate(Map.of()); }
        /** Send tenantValidateSession; fields and response retain their engine contracts. */
        public Object tenantValidateSession(Map<String, Object> options) { return call("tenantValidateSession", options, "principal"); }
        public Object tenantValidateSession() { return tenantValidateSession(Map.of()); }
        /** Send tenantRevokeSession; fields and response retain their engine contracts. */
        public Object tenantRevokeSession(Map<String, Object> options) { return call("tenantRevokeSession", options, "principal"); }
        public Object tenantRevokeSession() { return tenantRevokeSession(Map.of()); }
        /** Send tenantCheckAccess; fields and response retain their engine contracts. */
        public Object tenantCheckAccess(Map<String, Object> options) { return call("tenantCheckAccess", options, "principal"); }
        public Object tenantCheckAccess() { return tenantCheckAccess(Map.of()); }
        /** Send tenantGetStats; fields and response retain their engine contracts. */
        public Object tenantGetStats(Map<String, Object> options) { return call("tenantGetStats", options, "principal"); }
        public Object tenantGetStats() { return tenantGetStats(Map.of()); }
        /** Send tenantGetAuditLog; fields and response retain their engine contracts. */
        public Object tenantGetAuditLog(Map<String, Object> options) { return call("tenantGetAuditLog", options, "principal"); }
        public Object tenantGetAuditLog() { return tenantGetAuditLog(Map.of()); }
        /** Send tenantCreateDatabase; fields and response retain their engine contracts. */
        public Object tenantCreateDatabase(Map<String, Object> options) { return call("tenantCreateDatabase", options, "database"); }
        public Object tenantCreateDatabase() { return tenantCreateDatabase(Map.of()); }
        /** Send tenantDropDatabase; fields and response retain their engine contracts. */
        public Object tenantDropDatabase(Map<String, Object> options) { return call("tenantDropDatabase", options, "database"); }
        public Object tenantDropDatabase() { return tenantDropDatabase(Map.of()); }
        /** Send tenantListDatabases; fields and response retain their engine contracts. */
        public Object tenantListDatabases(Map<String, Object> options) { return call("tenantListDatabases", options, "principal"); }
        public Object tenantListDatabases() { return tenantListDatabases(Map.of()); }
        /** Send tenantCreateCollection; fields and response retain their engine contracts. */
        public Object tenantCreateCollection(Map<String, Object> options) { return call("tenantCreateCollection", options, "database"); }
        public Object tenantCreateCollection() { return tenantCreateCollection(Map.of()); }
        /** Send tenantDropCollection; fields and response retain their engine contracts. */
        public Object tenantDropCollection(Map<String, Object> options) { return call("tenantDropCollection", options, "database"); }
        public Object tenantDropCollection() { return tenantDropCollection(Map.of()); }
        /** Send register_node; fields and response retain their engine contracts. */
        public Object registerNode(Map<String, Object> options) { return call("register_node", options, "global"); }
        public Object registerNode() { return registerNode(Map.of()); }
        /** Send deregister_node; fields and response retain their engine contracts. */
        public Object deregisterNode(Map<String, Object> options) { return call("deregister_node", options, "global"); }
        public Object deregisterNode() { return deregisterNode(Map.of()); }
        /** Send create_shard; fields and response retain their engine contracts. */
        public Object createShard(Map<String, Object> options) { return call("create_shard", options, "global"); }
        public Object createShard() { return createShard(Map.of()); }
        /** Send migrate_shard; fields and response retain their engine contracts. */
        public Object migrateShard(Map<String, Object> options) { return call("migrate_shard", options, "global"); }
        public Object migrateShard() { return migrateShard(Map.of()); }
        /** Send rebalance_shards; fields and response retain their engine contracts. */
        public Object rebalanceShards(Map<String, Object> options) { return call("rebalance_shards", options, "global"); }
        public Object rebalanceShards() { return rebalanceShards(Map.of()); }
        /** Send cluster_route; fields and response retain their engine contracts. */
        public Object clusterRoute(Map<String, Object> options) { return call("cluster_route", options, "principal"); }
        public Object clusterRoute() { return clusterRoute(Map.of()); }
        /** Send storage_stats; fields and response retain their engine contracts. */
        public Object storageStats(Map<String, Object> options) { return call("storage_stats", options, "global"); }
        public Object storageStats() { return storageStats(Map.of()); }
        /** Send wal_status; fields and response retain their engine contracts. */
        public Object walStatus(Map<String, Object> options) { return call("wal_status", options, "global"); }
        public Object walStatus() { return walStatus(Map.of()); }
        /** Send admin_compaction_status; fields and response retain their engine contracts. */
        public Object compactionStatus(Map<String, Object> options) { return call("admin_compaction_status", options, "global"); }
        public Object compactionStatus() { return compactionStatus(Map.of()); }
        /** Send admin_replay_check; fields and response retain their engine contracts. */
        public Object replayCheck(Map<String, Object> options) { return call("admin_replay_check", options, "global"); }
        public Object replayCheck() { return replayCheck(Map.of()); }
        /** Send memtable_status; fields and response retain their engine contracts. */
        public Object memtableStatus(Map<String, Object> options) { return call("memtable_status", options, "global"); }
        public Object memtableStatus() { return memtableStatus(Map.of()); }
        /** Send sst_status; fields and response retain their engine contracts. */
        public Object sstStatus(Map<String, Object> options) { return call("sst_status", options, "global"); }
        public Object sstStatus() { return sstStatus(Map.of()); }
        /** Send storage_flush; fields and response retain their engine contracts. */
        public Object storageFlush(Map<String, Object> options) { return call("storage_flush", options, "global"); }
        public Object storageFlush() { return storageFlush(Map.of()); }
        /** Send storage_compact; fields and response retain their engine contracts. */
        public Object storageCompact(Map<String, Object> options) { return call("storage_compact", options, "global"); }
        public Object storageCompact() { return storageCompact(Map.of()); }
        /** Send storage_repair; fields and response retain their engine contracts. */
        public Object storageRepair(Map<String, Object> options) { return call("storage_repair", options, "global"); }
        public Object storageRepair() { return storageRepair(Map.of()); }
        /** Send verify_integrity; fields and response retain their engine contracts. */
        public Object verifyIntegrity(Map<String, Object> options) { return call("verify_integrity", options, "principal"); }
        public Object verifyIntegrity() { return verifyIntegrity(Map.of()); }
        /** Send lsm_metrics; fields and response retain their engine contracts. */
        public Object lsmMetrics(Map<String, Object> options) { return call("lsm_metrics", options, "global"); }
        public Object lsmMetrics() { return lsmMetrics(Map.of()); }
    }
}
