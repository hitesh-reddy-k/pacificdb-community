import { useState } from 'react';
import { Plus, Server, Zap, Database, Shield, Wifi } from 'lucide-react';
import { useConnectionStore } from '../../stores/connection-store';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { Button } from '../../components/ui/Button';
import { ConnectionEditor } from './ConnectionEditor';
import { clsx } from 'clsx';
import type { ConnectionProfile } from '../../../electron/protocol/types';

const ENV_COLORS: Record<string, string> = {
  local:      'badge-green',
  dev:        'badge-blue',
  staging:    'badge-yellow',
  production: 'badge-red',
};

export function WelcomePage() {
  const profiles = useConnectionStore(s => s.profiles);
  const statuses = useConnectionStore(s => s.statuses);
  const connect = useConnectionStore(s => s.connect);
  const deleteProfile = useConnectionStore(s => s.deleteProfile);
  const setActive = useWorkspaceStore(s => s.setActiveProfile);
  const navigateTo = useWorkspaceStore(s => s.navigateTo);
  const notify = useWorkspaceStore(s => s.notify);

  const [editorOpen, setEditorOpen] = useState(false);
  const [editingProfile, setEditingProfile] = useState<ConnectionProfile | null>(null);
  const [connecting, setConnecting] = useState<string | null>(null);

  const handleConnect = async (profileId: string) => {
    setConnecting(profileId);
    setActive(profileId);
    try {
      const status = await connect(profileId);
      if (status.state === 'connected' || status.state === 'authenticated') {
        notify({ type: 'success', title: `Connected to ${profiles.find(p => p.id === profileId)?.name}` });
        navigateTo('overview');
      }
    } catch (e) {
      notify({ type: 'error', title: 'Connection failed', message: String(e) });
    } finally {
      setConnecting(null);
    }
  };

  const handleEdit = (p: ConnectionProfile) => {
    setEditingProfile(p);
    setEditorOpen(true);
  };

  return (
    <div className="h-full overflow-auto bg-surface-900">
      {/* Hero */}
      <div className="border-b border-surface-500/30 bg-gradient-to-r from-surface-800 to-surface-900 px-8 py-8">
        <div className="flex items-center gap-3 mb-2">
          <div className="w-8 h-8 rounded-lg bg-gradient-to-br from-pacific-500 to-pacific-700 flex items-center justify-center shadow-lg">
            <Database size={16} className="text-white" />
          </div>
          <h1 className="text-2xl font-bold text-surface-50">PacificDB Workbench</h1>
        </div>
        <p className="text-sm text-surface-400 max-w-xl">
          A professional GUI client and administration environment for PacificDB Community Edition.
          Connect to any PacificDB server to browse, query, and manage your data.
        </p>
        <div className="flex items-center gap-3 mt-4">
          <div className="flex items-center gap-1.5 text-xs text-surface-400">
            <Zap size={12} className="text-pacific-400" /> Documents
          </div>
          <div className="flex items-center gap-1.5 text-xs text-surface-400">
            <Zap size={12} className="text-purple-400" /> Vectors
          </div>
          <div className="flex items-center gap-1.5 text-xs text-surface-400">
            <Zap size={12} className="text-success" /> Media
          </div>
          <div className="flex items-center gap-1.5 text-xs text-surface-400">
            <Shield size={12} className="text-warning" /> Security
          </div>
        </div>
      </div>

      {/* Content */}
      <div className="p-8 max-w-4xl">
        {/* Connections section */}
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-md font-semibold text-surface-100">Saved Connections</h2>
          <Button
            variant="primary"
            size="sm"
            icon={<Plus size={12} />}
            onClick={() => { setEditingProfile(null); setEditorOpen(true); }}
            id="add-connection-btn"
          >
            New Connection
          </Button>
        </div>

        {profiles.length === 0 ? (
          <div className="border border-dashed border-surface-500/40 rounded-xl p-12 text-center">
            <Server size={32} className="text-surface-600 mx-auto mb-3" />
            <p className="text-sm font-medium text-surface-300">No connections yet</p>
            <p className="text-xs text-surface-500 mt-1 mb-4">
              Add your first PacificDB server connection to get started.
            </p>
            <Button
              variant="primary"
              size="sm"
              icon={<Plus size={12} />}
              onClick={() => setEditorOpen(true)}
            >
              Add Connection
            </Button>
          </div>
        ) : (
          <div className="grid grid-cols-1 gap-3">
            {profiles.map(profile => {
              const status = statuses[profile.id];
              const connected = status?.state === 'connected' || status?.state === 'authenticated';
              const isConnecting = connecting === profile.id || status?.state === 'connecting';

              return (
                <div
                  key={profile.id}
                  className={clsx(
                    'card flex items-center gap-4 group cursor-default',
                    connected && 'border-pacific-500/30 bg-pacific-500/5',
                  )}
                >
                  <div className={clsx(
                    'w-10 h-10 rounded-lg flex items-center justify-center flex-shrink-0',
                    connected ? 'bg-pacific-500/20' : 'bg-surface-700',
                  )}>
                    <Server size={16} className={connected ? 'text-pacific-400' : 'text-surface-400'} />
                  </div>

                  <div className="flex-1 min-w-0">
                    <div className="flex items-center gap-2">
                      <p className="font-medium text-surface-100 text-sm">{profile.name}</p>
                      <span className={ENV_COLORS[profile.env] ?? 'badge-gray'}>{profile.env}</span>
                      {connected && (
                        <span className="badge-green">
                          <Wifi size={8} /> connected
                        </span>
                      )}
                    </div>
                    <p className="text-xs text-surface-400 mt-0.5 font-mono">
                      {profile.host}:{profile.port}
                      {profile.database ? ` · ${profile.database}` : ''}
                      {profile.tls?.enabled ? ' · TLS' : ''}
                    </p>
                    {profile.lastConnectedAt && (
                      <p className="text-2xs text-surface-500 mt-0.5">
                        Last connected {new Date(profile.lastConnectedAt).toLocaleString()}
                      </p>
                    )}
                  </div>

                  <div className="flex items-center gap-2 opacity-0 group-hover:opacity-100 transition-opacity">
                    <Button variant="ghost" size="xs" onClick={() => handleEdit(profile)} id={`edit-${profile.id}`}>Edit</Button>
                    <Button
                      variant="danger"
                      size="xs"
                      onClick={() => deleteProfile(profile.id)}
                      id={`delete-${profile.id}`}
                    >
                      Delete
                    </Button>
                  </div>

                  <Button
                    variant={connected ? 'ghost' : 'primary'}
                    size="sm"
                    loading={isConnecting}
                    icon={<Wifi size={12} />}
                    onClick={() => connected ? navigateTo('overview') : handleConnect(profile.id)}
                    id={`connect-${profile.id}`}
                  >
                    {connected ? 'Open' : 'Connect'}
                  </Button>
                </div>
              );
            })}
          </div>
        )}
      </div>

      <ConnectionEditor
        open={editorOpen}
        profile={editingProfile}
        onClose={() => setEditorOpen(false)}
      />
    </div>
  );
}
