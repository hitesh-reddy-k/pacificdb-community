import { useState, useEffect } from 'react';
import { CheckCircle2, XCircle, Loader2, Eye, EyeOff } from 'lucide-react';
import { Dialog } from '../../components/ui/Dialog';
import { Input, Select } from '../../components/ui/Input';
import { Button } from '../../components/ui/Button';
import { Tabs, TabPanel, useTabs } from '../../components/ui/Tabs';
import { useConnectionStore } from '../../stores/connection-store';
import { useWorkspaceStore } from '../../stores/workspace-store';
import type { ConnectionProfile } from '../../../electron/protocol/types';

interface Props {
  open: boolean;
  profile: ConnectionProfile | null;
  onClose: () => void;
}

const DEFAULT_PROFILE: Partial<ConnectionProfile> & { plainPassword?: string; plainToken?: string } = {
  name: '',
  host: 'localhost',
  port: 9000,
  env: 'local',
  auth: { mode: 'none' },
  tls: { enabled: false, verifyServer: true },
  advanced: { poolSize: 4, requestTimeoutMs: 30000, connectTimeoutMs: 10000 },
};

export function ConnectionEditor({ open, profile, onClose }: Props) {
  const { activeTab, setActiveTab } = useTabs('general');
  const createProfile = useConnectionStore(s => s.createProfile);
  const updateProfile = useConnectionStore(s => s.updateProfile);
  const testConnection = useConnectionStore(s => s.testConnection);
  const notify = useWorkspaceStore(s => s.notify);

  const [form, setForm] = useState({ ...DEFAULT_PROFILE });
  const [plainPassword, setPlainPassword] = useState('');
  const [plainToken, setPlainToken] = useState('');
  const [showPassword, setShowPassword] = useState(false);
  const [saving, setSaving] = useState(false);
  const [testing, setTesting] = useState(false);
  const [testResult, setTestResult] = useState<{ success: boolean; latencyMs: number; error?: string } | null>(null);

  useEffect(() => {
    if (profile) {
      setForm({ ...profile });
    } else {
      setForm({ ...DEFAULT_PROFILE });
    }
    setPlainPassword('');
    setPlainToken('');
    setTestResult(null);
    setActiveTab('general');
  }, [open, profile]);

  const merge = (updates: Partial<typeof form>) => setForm(f => ({ ...f, ...updates }));
  const mergeAuth = (updates: Partial<NonNullable<typeof form.auth>>) =>
    merge({ auth: { ...form.auth!, ...updates } });
  const mergeTls = (updates: Partial<NonNullable<typeof form.tls>>) =>
    merge({ tls: { ...form.tls!, ...updates } });
  const mergeAdv = (updates: Partial<NonNullable<typeof form.advanced>>) =>
    merge({ advanced: { ...form.advanced!, ...updates } });

  const handleTest = async () => {
    if (!form.host || !form.port) return;
    setTesting(true);
    setTestResult(null);
    try {
      // Create a temporary profile to test
      const tempId = await createProfile({
        ...(form as Omit<ConnectionProfile, 'id' | 'createdAt'>),
        name: '__test__',
        plainPassword: plainPassword || undefined,
        plainToken: plainToken || undefined,
      });
      const result = await testConnection(tempId.id);
      setTestResult(result);
      // Clean up temp profile
      await window.pacific.profiles.delete(tempId.id);
    } catch (e) {
      setTestResult({ success: false, latencyMs: 0, error: String(e) });
    } finally {
      setTesting(false);
    }
  };

  const handleSave = async () => {
    if (!form.name || !form.host || !form.port) {
      notify({ type: 'error', title: 'Name, host, and port are required' });
      return;
    }
    setSaving(true);
    try {
      if (profile) {
        await updateProfile(profile.id, {
          ...(form as Partial<ConnectionProfile>),
          ...(plainPassword ? { plainPassword } : {}),
          ...(plainToken ? { plainToken } : {}),
        });
        notify({ type: 'success', title: 'Connection updated' });
      } else {
        await createProfile({
          ...(form as Omit<ConnectionProfile, 'id' | 'createdAt'>),
          plainPassword: plainPassword || undefined,
          plainToken: plainToken || undefined,
        });
        notify({ type: 'success', title: 'Connection created' });
      }
      onClose();
    } catch (e) {
      notify({ type: 'error', title: 'Failed to save', message: String(e) });
    } finally {
      setSaving(false);
    }
  };

  const TABS = [
    { id: 'general', label: 'General' },
    { id: 'auth', label: 'Authentication' },
    { id: 'tls', label: 'TLS' },
    { id: 'advanced', label: 'Advanced' },
  ];

  return (
    <Dialog
      open={open}
      onClose={onClose}
      title={profile ? 'Edit Connection' : 'New Connection'}
      description="Configure PacificDB server connection settings"
      width="lg"
      id="connection-editor-dialog"
      footer={
        <>
          <Button variant="ghost" onClick={handleTest} loading={testing} size="sm" id="test-connection-btn">
            Test Connection
          </Button>
          <div className="flex-1" />
          {testResult && (
            <div className="flex items-center gap-1.5 text-xs mr-2">
              {testResult.success
                ? <><CheckCircle2 size={12} className="text-success" /><span className="text-success">{testResult.latencyMs}ms</span></>
                : <><XCircle size={12} className="text-danger" /><span className="text-danger truncate max-w-40">{testResult.error}</span></>}
            </div>
          )}
          <Button variant="outline" onClick={onClose} size="sm">Cancel</Button>
          <Button variant="primary" onClick={handleSave} loading={saving} size="sm" id="save-connection-btn">
            {profile ? 'Update' : 'Create'}
          </Button>
        </>
      }
    >
      <Tabs tabs={TABS} activeTab={activeTab} onChange={setActiveTab} size="sm">
        {/* General */}
        <TabPanel id="general" activeTab={activeTab} className="p-1">
          <div className="grid grid-cols-2 gap-3 mt-3">
            <div className="col-span-2">
              <Input
                label="Connection Name"
                id="conn-name"
                value={form.name ?? ''}
                onChange={e => merge({ name: e.target.value })}
                placeholder="My PacificDB Server"
                autoFocus
              />
            </div>
            <Input
              label="Host"
              id="conn-host"
              value={form.host ?? ''}
              onChange={e => merge({ host: e.target.value })}
              placeholder="localhost"
              className="font-mono"
            />
            <Input
              label="Port"
              id="conn-port"
              type="number"
              value={String(form.port ?? 9000)}
              onChange={e => merge({ port: Number(e.target.value) })}
              placeholder="9000"
              className="font-mono"
            />
            <Input
              label="Default Database (optional)"
              id="conn-database"
              value={form.database ?? ''}
              onChange={e => merge({ database: e.target.value })}
              placeholder="mydb"
              className="font-mono"
            />
            <Select
              label="Environment"
              id="conn-env"
              value={form.env ?? 'local'}
              onChange={e => merge({ env: e.target.value as ConnectionProfile['env'] })}
            >
              <option value="local">Local</option>
              <option value="dev">Development</option>
              <option value="staging">Staging</option>
              <option value="production">Production</option>
            </Select>
          </div>
        </TabPanel>

        {/* Authentication */}
        <TabPanel id="auth" activeTab={activeTab} className="p-1">
          <div className="flex flex-col gap-3 mt-3">
            <Select
              label="Authentication Mode"
              id="auth-mode"
              value={form.auth?.mode ?? 'none'}
              onChange={e => mergeAuth({ mode: e.target.value as NonNullable<ConnectionProfile['auth']>['mode'] })}
            >
              <option value="none">None (no authentication)</option>
              <option value="password">Username / Password</option>
              <option value="token">Bearer Token</option>
              <option value="apikey">API Key</option>
            </Select>

            {form.auth?.mode === 'password' && (
              <>
                <Input
                  label="User ID (optional)"
                  id="auth-userid"
                  value={form.auth?.userId ?? ''}
                  onChange={e => mergeAuth({ userId: e.target.value })}
                  placeholder="system"
                />
                <Input
                  label="Username"
                  id="auth-username"
                  value={form.auth?.username ?? ''}
                  onChange={e => mergeAuth({ username: e.target.value })}
                  placeholder="admin"
                  autoComplete="username"
                />
                <div className="relative">
                  <Input
                    label="Password"
                    id="auth-password"
                    type={showPassword ? 'text' : 'password'}
                    value={plainPassword}
                    onChange={e => setPlainPassword(e.target.value)}
                    placeholder={profile?.auth?.encryptedPassword ? '••••••••' : 'Enter password'}
                    autoComplete="current-password"
                  />
                  <button
                    type="button"
                    onClick={() => setShowPassword(v => !v)}
                    className="absolute right-2 bottom-2 p-0.5 text-surface-400 hover:text-surface-200"
                  >
                    {showPassword ? <EyeOff size={13} /> : <Eye size={13} />}
                  </button>
                </div>
                {profile?.auth?.encryptedPassword && !plainPassword && (
                  <p className="text-2xs text-surface-400">
                    Password stored securely. Enter a new password to update it.
                  </p>
                )}
              </>
            )}

            {(form.auth?.mode === 'token' || form.auth?.mode === 'apikey') && (
              <div className="relative">
                <Input
                  label={form.auth.mode === 'apikey' ? 'API Key' : 'Bearer Token'}
                  id="auth-token"
                  type={showPassword ? 'text' : 'password'}
                  value={plainToken}
                  onChange={e => setPlainToken(e.target.value)}
                  placeholder={profile?.auth?.encryptedToken ? '••••••••' : 'Enter token'}
                  className="font-mono"
                />
                <button
                  type="button"
                  onClick={() => setShowPassword(v => !v)}
                  className="absolute right-2 bottom-2 p-0.5 text-surface-400 hover:text-surface-200"
                >
                  {showPassword ? <EyeOff size={13} /> : <Eye size={13} />}
                </button>
              </div>
            )}
          </div>
        </TabPanel>

        {/* TLS */}
        <TabPanel id="tls" activeTab={activeTab} className="p-1">
          <div className="flex flex-col gap-3 mt-3">
            <label className="flex items-center gap-2 cursor-pointer">
              <input
                type="checkbox"
                id="tls-enabled"
                checked={form.tls?.enabled ?? false}
                onChange={e => mergeTls({ enabled: e.target.checked })}
                className="accent-pacific-500"
              />
              <span className="text-sm text-surface-200">Enable TLS/SSL</span>
            </label>
            {form.tls?.enabled && (
              <>
                <label className="flex items-center gap-2 cursor-pointer">
                  <input
                    type="checkbox"
                    id="tls-verify"
                    checked={form.tls?.verifyServer ?? true}
                    onChange={e => mergeTls({ verifyServer: e.target.checked })}
                    className="accent-pacific-500"
                  />
                  <span className="text-sm text-surface-200">Verify server certificate</span>
                </label>
                <Input
                  label="CA Certificate Path (optional)"
                  id="tls-ca"
                  value={form.tls?.caPath ?? ''}
                  onChange={e => mergeTls({ caPath: e.target.value })}
                  placeholder="/path/to/ca.pem"
                  className="font-mono"
                />
                <Input
                  label="Client Certificate Path (optional)"
                  id="tls-cert"
                  value={form.tls?.certPath ?? ''}
                  onChange={e => mergeTls({ certPath: e.target.value })}
                  placeholder="/path/to/client.pem"
                  className="font-mono"
                />
                <Input
                  label="Client Key Path (optional)"
                  id="tls-key"
                  value={form.tls?.keyPath ?? ''}
                  onChange={e => mergeTls({ keyPath: e.target.value })}
                  placeholder="/path/to/client-key.pem"
                  className="font-mono"
                />
              </>
            )}
          </div>
        </TabPanel>

        {/* Advanced */}
        <TabPanel id="advanced" activeTab={activeTab} className="p-1">
          <div className="flex flex-col gap-3 mt-3">
            <Input
              label="Connection Pool Size"
              id="adv-pool"
              type="number"
              value={String(form.advanced?.poolSize ?? 4)}
              onChange={e => mergeAdv({ poolSize: Number(e.target.value) })}
              hint="Number of TCP connections in pool (1–32)"
            />
            <Input
              label="Request Timeout (ms)"
              id="adv-timeout"
              type="number"
              value={String(form.advanced?.requestTimeoutMs ?? 30000)}
              onChange={e => mergeAdv({ requestTimeoutMs: Number(e.target.value) })}
              hint="Per-request timeout in milliseconds"
            />
            <Input
              label="Connect Timeout (ms)"
              id="adv-conn-timeout"
              type="number"
              value={String(form.advanced?.connectTimeoutMs ?? 10000)}
              onChange={e => mergeAdv({ connectTimeoutMs: Number(e.target.value) })}
              hint="TCP connection timeout in milliseconds"
            />
          </div>
        </TabPanel>
      </Tabs>
    </Dialog>
  );
}
