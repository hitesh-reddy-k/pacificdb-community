# Community SDK capability coverage

This source candidate maps all **135** actual actions in the Community server's
main dispatch chain to named Python and Java methods: 117 canonical actions and
18 wire aliases. The matrix is
[`community-capabilities.json`](../sdk/contracts/community-capabilities.json).
The validator checks dispatch coverage, duplicate entries, alias bindings and
method presence; language wire tests invoke each binding with successful and
failed replies. These tests do **not** certify all advanced operations on a live
cluster. Authenticated integration and package qualification are separate gates.

The beginner client has database/collection lifecycle and document CRUD, bulk,
count, aggregate and explain methods. Advanced families are `db.indexes`,
`db.vectors`, `db.media`, `db.backups`, `db.security`, `db.admin` in Python, and
`db.indexes()`, etc. in Java. Their methods take engine fields as Python keyword
arguments or a Java `Map<String,Object>`. Java also provides no-argument
convenience calls. Responses retain engine shape, errors retain sanitized engine
codes/details, and no unsupported feature is emulated.

```python
# db is an open PacificDB client with a selected database
print(db.indexes.create(collection='users', fields={'name': 1}))
print(db.vectors.query(collection='vectors', vector=[1.0, 0.0], k=5))
print(db.security.whoami())
print(db.admin.health_check())
```

```java
System.out.println(db.indexes().create(Map.of("collection", "users", "fields", Map.of("name", 1))));
System.out.println(db.vectors().query(Map.of("collection", "vectors", "vector", List.of(1.0, 0.0), "k", 5)));
System.out.println(db.security().whoami());
System.out.println(db.admin().healthCheck());
```

Family options cannot replace `action`, `userId`, `token`, `dbName`, or its `db`
alias. Those values come from the captured client scope; use the beginner
selection methods or a separate client for another database. Named target
fields such as catalog `database` and restore `target_dir` remain explicit engine
arguments. Collection/database scoped methods require a selected database;
other methods capture selection but operate at their documented engine scope.

`security.authenticate` / `security().authenticate` store a returned valid token.
`refresh_token` / `refreshToken` replace it only after successful validation of
the response. `security.use_token(value)` / `security().useToken(value)` explicitly
install a token or API key without a network call. The validate/whoami and legacy
tenant session operations use that captured token; use a separate client for
legacy tenant sessions so they do not replace another client's engine identity.
A token supplied in ordinary family options is rejected. Raw `request` remains
the explicit protocol escape hatch and permits deliberate scope overrides.

Legacy project/catalog methods remain under `admin` for compatibility. They do
not appear in beginner flows, do not implicitly select a project/database, and
require no data migration. Tenant, configuration, shard, replication and storage
methods expose existing manual engine commands. They do not implement automatic distributed
autoscaling, orchestration, automatic rebalancing, or new durability guarantees.
The current engine certifies only non-unique, single-field B-tree indexes; it
rejects unsupported index definitions. Vector operations require numeric vectors
in supported collections. The server remains authoritative for permissions,
reserved namespaces, configured edition restrictions and topology validity.

**Destructive operations:** database/collection/index drops, document deletion,
media cleanup/deletion, backup deletion/restore, tenant/user removal, token/key
revocation, configuration changes and storage/cluster maintenance can alter data
or availability. Global storage flush/compact/repair operates beyond the selected
collection. A backup restore target is a path on the server, not on the client's
machine. Read/ADMIN/backup permissions and feature prerequisites are enforced by
the engine. Do not invoke maintenance against an unrelated existing server.

Media `get_manifest` / `getManifest` is the low-level manifest action; bounded
file and in-memory convenience APIs are a separate implementation task. Media
`finalize_upload` / `finalizeUpload` avoids Java's `Object.finalize()` name collision.
Alias entries below share a canonical method and **send the canonical wire
action**. To send an alias verbatim and retain its exact echoed action, use raw
protocol access. No actual dispatch is classified internal in this matrix;
permission-list names without a handler are not advertised as supported actions.

In the table, explicit required fields omit the principal/token and selected
`dbName` supplied by scope. The JSON matrix also supplies executable sample
fields. Optional engine arguments can be passed by their exact JSON names.

| Engine action | Python method | Java method | Scope | Required explicit fields |
| --- | --- | --- | --- | --- |
| `ping` | `admin.ping` | `admin.ping` | principal | — |
| `community_capabilities` | `capabilities` | `capabilities` | principal | — |
| `community_project_create` | `admin.community_project_create` | `admin.communityProjectCreate` | principal | `name` |
| `community_project_list` | `admin.community_project_list` | `admin.communityProjectList` | principal | — |
| `community_project_get` | `admin.community_project_get` | `admin.communityProjectGet` | principal | `id` |
| `community_project_delete` | `admin.community_project_delete` | `admin.communityProjectDelete` | principal | `id` |
| `community_database_map` | `admin.community_database_map` | `admin.communityDatabaseMap` | principal | `project_id`, `database` |
| `community_database_list` | `admin.community_database_list` | `admin.communityDatabaseList` | principal | `project_id` |
| `community_database_project` | `admin.community_database_project` | `admin.communityDatabaseProject` | principal | `database` |
| `community_media_begin` | `media.begin` | `media.begin` | database | `collection`, `filename`, `size_bytes`, `chunk_count`, `sha256` |
| `community_media_put_chunk` | `media.put_chunk` | `media.putChunk` | principal | `media_id`, `index`, `data`, `size_bytes`, `sha256` |
| `community_media_finalize` | `media.finalize_upload` | `media.finalizeUpload` | principal | `media_id` |
| `community_media_list` | `media.list` | `media.list` | database | — |
| `community_media_get` | `media.get_manifest` | `media.getManifest` | principal | `media_id` |
| `community_media_get_chunk` | `media.get_chunk` | `media.getChunk` | principal | `media_id`, `index` |
| `community_media_delete` | `media.delete` | `media.delete` | principal | `media_id` |
| `community_media_cleanup` | `media.cleanup` | `media.cleanup` | principal | — |
| `observeLeaderTerm` | `admin.observe_leader_term` | `admin.observeLeaderTerm` | principal | — |
| `observe_leader_term → observeLeaderTerm` | `admin.observe_leader_term` | `admin.observeLeaderTerm` | principal | — |
| `security_authenticate` | `security.authenticate` | `security.authenticate` | principal | `username`, `password` |
| `security_validate_token` | `security.validate_token` | `security.validateToken` | principal | — |
| `security_whoami` | `security.whoami` | `security.whoami` | principal | — |
| `api_key_create` | `security.create_api_key` | `security.createApiKey` | principal | — |
| `api_key_list` | `security.list_api_keys` | `security.listApiKeys` | principal | — |
| `api_key_get` | `security.get_api_key` | `security.getApiKey` | principal | `id` |
| `api_key_revoke` | `security.revoke_api_key` | `security.revokeApiKey` | principal | `id` |
| `security_refresh_token` | `security.refresh_token` | `security.refreshToken` | principal | — |
| `security_metrics` | `security.metrics` | `security.metrics` | principal | — |
| `create_backup` | `backups.create` | `backups.create` | global | — |
| `list_backups` | `backups.list` | `backups.list` | principal | — |
| `get_backup` | `backups.get` | `backups.get` | principal | `backup_id` |
| `export_backup_manifest` | `backups.export_manifest` | `backups.exportManifest` | principal | `backup_id` |
| `export_backup_file_chunk` | `backups.export_file_chunk` | `backups.exportFileChunk` | principal | `backup_id`, `path`, `offset` |
| `delete_backup` | `backups.delete` | `backups.delete` | global | `backup_id` |
| `list_restores` | `backups.list_restores` | `backups.listRestores` | principal | — |
| `verify_backup` | `backups.verify` | `backups.verify` | principal | `backup_id` |
| `restore_backup` | `backups.restore` | `backups.restore` | global | `backup_id` |
| `initUserSpace` | `admin.init_user_space` | `admin.initUserSpace` | principal | — |
| `createDatabase` | `create_database` | `createDatabase` | explicit_database | `dbName` |
| `createCollection` | `create_collection` | `createCollection` | collection | `collection` |
| `dropDatabase` | `drop_database` | `dropDatabase` | explicit_database | `dbName` |
| `dropCollection` | `drop_collection` | `dropCollection` | collection | `collection` |
| `listCollections` | `list_collections` | `listCollections` | database | — |
| `createIndex` | `indexes.create` | `indexes.create` | collection | `collection`, `fields` |
| `listIndexes` | `indexes.list` | `indexes.list` | collection | `collection` |
| `validateIndex` | `indexes.validate` | `indexes.validate` | collection | `collection` |
| `indexValidate → validateIndex` | `indexes.validate` | `indexes.validate` | collection | `collection` |
| `rebuildIndex` | `indexes.rebuild` | `indexes.rebuild` | collection | `collection`, `name` |
| `indexRebuild → rebuildIndex` | `indexes.rebuild` | `indexes.rebuild` | collection | `collection`, `name` |
| `dropIndex` | `indexes.drop` | `indexes.drop` | collection | `collection`, `name` |
| `listDatabases` | `list_databases` | `listDatabases` | principal | — |
| `insert` | `insert` | `insert` | collection | `collection`, `data` |
| `insertVector` | `vectors.insert` | `vectors.insert` | collection | `collection`, `data` |
| `count` | `count` | `count` | collection | `collection` |
| `aggregate` | `aggregate` | `aggregate` | collection | `collection`, `pipeline` |
| `explain` | `explain` | `explain` | collection | `collection` |
| `find` | `find` | `find` | collection | `collection` |
| `queryVector` | `vectors.query` | `vectors.query` | collection | `collection`, `vector` |
| `updateOne` | `update_one` | `updateOne` | collection | `collection`, `filter`, `update` |
| `update → updateOne` | `update_one` | `updateOne` | collection | `collection`, `filter`, `update` |
| `deleteOne` | `delete_one` | `deleteOne` | collection | `collection`, `filter` |
| `insertMany` | `insert_many` | `insertMany` | collection | `collection`, `data` |
| `updateMany` | `update_many` | `updateMany` | collection | `collection`, `filter`, `update` |
| `deleteMany` | `delete_many` | `deleteMany` | collection | `collection`, `filter` |
| `bulk → bulkWrite` | `bulk_write` | `bulkWrite` | collection | `collection`, `ops` |
| `bulkWrite` | `bulk_write` | `bulkWrite` | collection | `collection`, `ops` |
| `opStatus` | `admin.op_status` | `admin.opStatus` | principal | `opId` |
| `get_metrics` | `admin.get_metrics` | `admin.getMetrics` | principal | — |
| `health_check` | `admin.health_check` | `admin.healthCheck` | principal | — |
| `get_cluster_status` | `admin.get_cluster_status` | `admin.getClusterStatus` | principal | — |
| `split_shard` | `admin.split_shard` | `admin.splitShard` | global | `shardId`, `splitKey` |
| `get_shard_details` | `admin.get_shard_details` | `admin.getShardDetails` | principal | — |
| `list_shards` | `admin.list_shards` | `admin.listShards` | principal | — |
| `config_get` | `admin.config_get` | `admin.configGet` | global | — |
| `config_set` | `admin.config_set` | `admin.configSet` | global | `key` |
| `config_reload` | `admin.config_reload` | `admin.configReload` | global | — |
| `config_dump` | `admin.config_dump` | `admin.configDump` | global | — |
| `admin_raft_status` | `admin.raft_status` | `admin.raftStatus` | principal | — |
| `admin_replication_status → admin_raft_status` | `admin.raft_status` | `admin.raftStatus` | principal | — |
| `admin_replica_digest` | `admin.replica_digest` | `admin.replicaDigest` | collection | `collection`, `fence`, `maxDocs` |
| `admin_apply_status` | `admin.apply_status` | `admin.applyStatus` | principal | — |
| `admin_logical_write_status` | `admin.logical_write_status` | `admin.logicalWriteStatus` | collection | `collection`, `logicalWriteId` |
| `getWriteStatus → admin_logical_write_status` | `admin.logical_write_status` | `admin.logicalWriteStatus` | collection | `collection`, `logicalWriteId` |
| `admin_storage_visibility_check` | `admin.storage_visibility_check` | `admin.storageVisibilityCheck` | collection | `collection`, `filter` |
| `admin_dashboard` | `admin.dashboard` | `admin.dashboard` | principal | — |
| `createTenant` | `admin.create_tenant` | `admin.createTenant` | principal | `tenantId`, `ownerPassword` |
| `deleteTenant` | `admin.delete_tenant` | `admin.deleteTenant` | principal | `tenantId`, `confirmedBy` |
| `listTenants` | `admin.list_tenants` | `admin.listTenants` | principal | — |
| `getTenant` | `admin.get_tenant` | `admin.getTenant` | principal | `tenantId` |
| `updateTenant` | `admin.update_tenant` | `admin.updateTenant` | principal | `tenantId` |
| `createTenantUser` | `admin.create_tenant_user` | `admin.createTenantUser` | principal | `tenantId`, `username`, `password`, `createdBy` |
| `deleteTenantUser` | `admin.delete_tenant_user` | `admin.deleteTenantUser` | principal | `tenantId`, `username`, `deletedBy` |
| `updateTenantUserRole` | `admin.update_tenant_user_role` | `admin.updateTenantUserRole` | principal | `tenantId`, `username`, `role`, `updatedBy` |
| `updateTenantUserPassword` | `admin.update_tenant_user_password` | `admin.updateTenantUserPassword` | principal | `tenantId`, `username`, `newPassword`, `updatedBy` |
| `setTenantUserDatabaseAccess` | `admin.set_tenant_user_database_access` | `admin.setTenantUserDatabaseAccess` | principal | `tenantId`, `username`, `updatedBy` |
| `listTenantUsers` | `admin.list_tenant_users` | `admin.listTenantUsers` | principal | `tenantId` |
| `tenantAuthenticate` | `admin.tenant_authenticate` | `admin.tenantAuthenticate` | principal | `tenantId`, `username`, `password` |
| `tenantValidateSession` | `admin.tenant_validate_session` | `admin.tenantValidateSession` | principal | — |
| `tenantRevokeSession` | `admin.tenant_revoke_session` | `admin.tenantRevokeSession` | principal | — |
| `tenantCheckAccess` | `admin.tenant_check_access` | `admin.tenantCheckAccess` | principal | `targetAction` |
| `tenantGetStats` | `admin.tenant_get_stats` | `admin.tenantGetStats` | principal | — |
| `tenantGetAuditLog` | `admin.tenant_get_audit_log` | `admin.tenantGetAuditLog` | principal | `tenantId` |
| `tenantCreateDatabase` | `admin.tenant_create_database` | `admin.tenantCreateDatabase` | database | — |
| `tenantDropDatabase` | `admin.tenant_drop_database` | `admin.tenantDropDatabase` | database | — |
| `tenantListDatabases` | `admin.tenant_list_databases` | `admin.tenantListDatabases` | principal | — |
| `tenantCreateCollection` | `admin.tenant_create_collection` | `admin.tenantCreateCollection` | database | `collName` |
| `tenantDropCollection` | `admin.tenant_drop_collection` | `admin.tenantDropCollection` | database | `collName` |
| `register_node` | `admin.register_node` | `admin.registerNode` | global | `nodeId` |
| `cluster_register → register_node` | `admin.register_node` | `admin.registerNode` | global | `nodeId` |
| `deregister_node` | `admin.deregister_node` | `admin.deregisterNode` | global | `nodeId` |
| `cluster_deregister → deregister_node` | `admin.deregister_node` | `admin.deregisterNode` | global | `nodeId` |
| `create_shard` | `admin.create_shard` | `admin.createShard` | global | — |
| `migrate_shard` | `admin.migrate_shard` | `admin.migrateShard` | global | `shardId`, `targetNode` |
| `rebalance_shards` | `admin.rebalance_shards` | `admin.rebalanceShards` | global | — |
| `cluster_route` | `admin.cluster_route` | `admin.clusterRoute` | principal | `key` |
| `route_key → cluster_route` | `admin.cluster_route` | `admin.clusterRoute` | principal | `key` |
| `storage_stats` | `admin.storage_stats` | `admin.storageStats` | global | — |
| `wal_status` | `admin.wal_status` | `admin.walStatus` | global | — |
| `admin_wal_status → wal_status` | `admin.wal_status` | `admin.walStatus` | global | — |
| `admin_lsm_status → lsm_metrics` | `admin.lsm_metrics` | `admin.lsmMetrics` | global | — |
| `admin_compaction_status` | `admin.compaction_status` | `admin.compactionStatus` | global | — |
| `admin_replay_check` | `admin.replay_check` | `admin.replayCheck` | global | — |
| `admin_storage_verify → verify_integrity` | `admin.verify_integrity` | `admin.verifyIntegrity` | principal | — |
| `memtable_status` | `admin.memtable_status` | `admin.memtableStatus` | global | — |
| `sst_status` | `admin.sst_status` | `admin.sstStatus` | global | — |
| `storage_flush` | `admin.storage_flush` | `admin.storageFlush` | global | — |
| `flush → storage_flush` | `admin.storage_flush` | `admin.storageFlush` | global | — |
| `storage_compact` | `admin.storage_compact` | `admin.storageCompact` | global | — |
| `compact → storage_compact` | `admin.storage_compact` | `admin.storageCompact` | global | — |
| `storage_repair` | `admin.storage_repair` | `admin.storageRepair` | global | — |
| `repair → storage_repair` | `admin.storage_repair` | `admin.storageRepair` | global | — |
| `verify_integrity` | `admin.verify_integrity` | `admin.verifyIntegrity` | principal | — |
| `lsm_metrics` | `admin.lsm_metrics` | `admin.lsmMetrics` | global | — |
| `lsmProfile → lsm_metrics` | `admin.lsm_metrics` | `admin.lsmMetrics` | global | — |
| `lsm_profile → lsm_metrics` | `admin.lsm_metrics` | `admin.lsmMetrics` | global | — |
