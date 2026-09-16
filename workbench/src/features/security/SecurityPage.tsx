import { useState, useEffect, useCallback } from 'react';
import { Shield, Plus, Trash2, RefreshCw, Eye, EyeOff, Copy } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Input, Select } from '../../components/ui/Input';
import { Dialog, ConfirmDialog } from '../../components/ui/Dialog';
import { EmptyState, SkeletonBlock } from '../../components/ui/EmptyState';
import { Tabs, TabPanel, useTabs } from '../../components/ui/Tabs';

interface ApiKey {
  id: string;
  name?: string;
  role?: string;
  created_at?: string;
  last_used?: string;
}

const ROLE_BADGES: Record<string, string> = {
  admin: 'badge-red',
  readwrite: 'badge-blue',
  read: 'badge-green',
  read_only: 'badge-green',
  backup_operator: 'badge-yellow',
  metrics_viewer: 'badge-gray',
};

export function SecurityPage() {
  const notify = useWorkspaceStore(s => s.notify);
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const { request } = usePacific();
  const { activeTab, setActiveTab } = useTabs('api-keys');

  const [apiKeys, setApiKeys] = useState<ApiKey[]>([]);
  const [loading, setLoading] = useState(false);
  const [createOpen, setCreateOpen] = useState(false);
  const [newKeyName, setNewKeyName] = useState('');
  const [newKeyRole, setNewKeyRole] = useState('readwrite');
  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState('');
  const [newKeyResult, setNewKeyResult] = useState<{ key?: string; id?: string } | null>(null);
  const [showKey, setShowKey] = useState(false);
  const [revokeTarget, setRevokeTarget] = useState<ApiKey | null>(null);
  const [revoking, setRevoking] = useState(false);
  const [whoami, setWhoami] = useState<Record<string, unknown> | null>(null);

  const loadApiKeys = useCallback(async () => {
    setLoading(true);
    try {
      const { response } = await request<{ keys?: ApiKey[] }>({ action: 'api_key_list' });
      setApiKeys(response.keys ?? []);
    } catch { setApiKeys([]); }
    finally { setLoading(false); }
  }, [request]);

  const loadWhoami = useCallback(async () => {
    try {
      const { response } = await request<Record<string, unknown>>({ action: 'security_whoami' });
      setWhoami(response);
    } catch { setWhoami(null); }
  }, [request]);

  useEffect(() => { loadApiKeys(); loadWhoami(); }, [activeProfileId]);

  const handleCreateKey = async () => {
    if (!newKeyName.trim()) { setCreateError('Name is required'); return; }
    setCreating(true); setCreateError('');
    try {
      const { response } = await request<{ key?: string; id?: string }>({
        action: 'api_key_create',
        name: newKeyName.trim(),
        role: newKeyRole,
      });
      setNewKeyResult(response);
      notify({ type: 'success', title: 'API key created', message: 'Save the key now — it will not be shown again.' });
      await loadApiKeys();
    } catch (e) { setCreateError(String(e)); }
    finally { setCreating(false); }
  };

  const handleRevoke = async () => {
    if (!revokeTarget) return;
    setRevoking(true);
    try {
      await request({ action: 'api_key_revoke', id: revokeTarget.id });
      notify({ type: 'success', title: `Key "${revokeTarget.name ?? revokeTarget.id}" revoked` });
      setRevokeTarget(null);
      await loadApiKeys();
    } catch (e) {
      notify({ type: 'error', title: 'Revoke failed', message: String(e) });
    } finally {
      setRevoking(false);
    }
  };

  const copyKey = (k: string) => {
    navigator.clipboard.writeText(k);
    notify({ type: 'success', title: 'Key copied', duration: 2000 });
  };

  const TABS = [
    { id: 'api-keys', label: 'API Keys' },
    { id: 'identity', label: 'Identity' },
  ];

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <Shield size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Security</span>
      </div>

      <Tabs tabs={TABS} activeTab={activeTab} onChange={setActiveTab}>
        {/* API Keys */}
        <TabPanel id="api-keys" activeTab={activeTab} className="flex flex-col h-full">
          <div className="flex items-center justify-end gap-2 px-4 py-2 border-b border-surface-500/30">
            <Button variant="ghost" size="xs" icon={<RefreshCw size={11} />} onClick={loadApiKeys} loading={loading}>Refresh</Button>
            <Button variant="primary" size="xs" icon={<Plus size={11} />} onClick={() => setCreateOpen(true)} id="create-api-key-btn">New API Key</Button>
          </div>
          <div className="flex-1 overflow-auto p-4">
            {loading ? <SkeletonBlock rows={4} /> : apiKeys.length === 0 ? (
              <EmptyState
                icon={<Shield size={18} />}
                title="No API keys"
                description="Create an API key to grant programmatic access."
                action={<Button variant="primary" size="sm" icon={<Plus size={12} />} onClick={() => setCreateOpen(true)}>Create API Key</Button>}
              />
            ) : (
              <div className="space-y-2">
                {apiKeys.map(k => (
                  <div key={k.id} className="card flex items-center gap-4">
                    <div className="p-2 rounded-lg bg-surface-700"><Shield size={13} className="text-pacific-400" /></div>
                    <div className="flex-1 min-w-0">
                      <div className="flex items-center gap-2">
                        <p className="text-sm font-semibold text-surface-100">{k.name ?? k.id}</p>
                        {k.role && <span className={ROLE_BADGES[k.role] ?? 'badge-gray'}>{k.role}</span>}
                      </div>
                      <p className="text-xs text-surface-400 mt-0.5 font-mono">
                        {k.id}
                        {k.created_at ? ` · Created ${new Date(k.created_at).toLocaleDateString()}` : ''}
                        {k.last_used ? ` · Last used ${new Date(k.last_used).toLocaleDateString()}` : ''}
                      </p>
                    </div>
                    <Button variant="danger" size="xs" icon={<Trash2 size={11} />} onClick={() => setRevokeTarget(k)} id={`revoke-${k.id}`}>Revoke</Button>
                  </div>
                ))}
              </div>
            )}
          </div>
        </TabPanel>

        {/* Identity */}
        <TabPanel id="identity" activeTab={activeTab} className="p-4">
          {whoami ? (
            <div className="card max-w-md">
              <h2 className="text-sm font-semibold text-surface-200 mb-3 flex items-center gap-2">
                <Shield size={13} className="text-pacific-400" /> Current Identity
              </h2>
              <pre className="text-xs text-surface-300 font-mono selectable whitespace-pre-wrap">
                {JSON.stringify(whoami, null, 2)}
              </pre>
            </div>
          ) : (
            <EmptyState icon={<Shield size={18} />} title="Not authenticated" description="Connect with credentials to see identity." compact />
          )}
        </TabPanel>
      </Tabs>

      {/* Create key dialog */}
      <Dialog
        open={createOpen}
        onClose={() => { setCreateOpen(false); setNewKeyResult(null); setNewKeyName(''); setCreateError(''); }}
        title={newKeyResult ? 'API Key Created' : 'Create API Key'}
        width="sm"
        id="create-api-key-dialog"
        footer={
          newKeyResult ? (
            <Button variant="primary" size="sm" onClick={() => { setCreateOpen(false); setNewKeyResult(null); setNewKeyName(''); }}>Done</Button>
          ) : (
            <>
              <Button variant="outline" size="sm" onClick={() => setCreateOpen(false)}>Cancel</Button>
              <Button variant="primary" size="sm" onClick={handleCreateKey} loading={creating} id="confirm-create-key-btn">Create</Button>
            </>
          )
        }
      >
        {newKeyResult ? (
          <div className="flex flex-col gap-3">
            <div className="bg-warning/10 border border-warning/30 rounded-lg p-3 text-xs text-warning">
              ⚠ Save this key now. It will not be shown again.
            </div>
            <div className="relative">
              <div className="input font-mono text-xs break-all selectable pr-16 min-h-12">
                {showKey ? (newKeyResult.key ?? '') : '••••••••••••••••'}
              </div>
              <div className="absolute right-1 top-1 flex gap-1">
                <button onClick={() => setShowKey(v => !v)} className="p-1.5 rounded hover:bg-surface-600 text-surface-400">
                  {showKey ? <EyeOff size={12} /> : <Eye size={12} />}
                </button>
                <button onClick={() => copyKey(newKeyResult.key ?? '')} className="p-1.5 rounded hover:bg-surface-600 text-surface-400">
                  <Copy size={12} />
                </button>
              </div>
            </div>
          </div>
        ) : (
          <div className="flex flex-col gap-3">
            <Input label="Key Name" id="new-key-name" value={newKeyName} onChange={e => setNewKeyName(e.target.value)} placeholder="my-application" autoFocus />
            <Select label="Role" id="new-key-role" value={newKeyRole} onChange={e => setNewKeyRole(e.target.value)}>
              <option value="read_only">Read Only</option>
              <option value="readwrite">Read / Write</option>
              <option value="admin">Admin</option>
              <option value="backup_operator">Backup Operator</option>
              <option value="metrics_viewer">Metrics Viewer</option>
            </Select>
            {createError && <p className="text-xs text-danger">{createError}</p>}
          </div>
        )}
      </Dialog>

      <ConfirmDialog
        open={!!revokeTarget}
        onClose={() => setRevokeTarget(null)}
        onConfirm={handleRevoke}
        title="Revoke API Key"
        message={<>Revoke key <span className="code">{revokeTarget?.name ?? revokeTarget?.id}</span>? Applications using this key will lose access.</>}
        confirmLabel="Revoke Key"
        loading={revoking}
        id="revoke-key-confirm"
      />
    </div>
  );
}
