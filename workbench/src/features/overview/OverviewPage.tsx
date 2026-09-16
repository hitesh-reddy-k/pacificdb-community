import { useEffect, useState, useCallback } from 'react';
import { Activity, Server, Database, Cpu, Wifi, Shield, CheckCircle2, XCircle, RefreshCw } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { useConnectionStore } from '../../stores/connection-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { SkeletonBlock } from '../../components/ui/EmptyState';

interface StatCard {
  label: string;
  value: string | number;
  sub?: string;
  icon: React.ReactNode;
  accent?: string;
}

function StatCard({ label, value, sub, icon, accent = 'text-pacific-400' }: StatCard) {
  return (
    <div className="card flex items-start gap-3">
      <div className={`p-2 rounded-lg bg-surface-700 ${accent}`}>{icon}</div>
      <div>
        <p className="text-2xs text-surface-400 uppercase tracking-wide">{label}</p>
        <p className="text-xl font-bold text-surface-50 mt-0.5">{value}</p>
        {sub && <p className="text-xs text-surface-400 mt-0.5">{sub}</p>}
      </div>
    </div>
  );
}

export function OverviewPage() {
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const statuses = useConnectionStore(s => s.statuses);
  const { request } = usePacific();
  const notify = useWorkspaceStore(s => s.notify);
  const navigateTo = useWorkspaceStore(s => s.navigateTo);

  const [ping, setPing] = useState<Record<string, unknown> | null>(null);
  const [capabilities, setCapabilities] = useState<Record<string, unknown> | null>(null);
  const [databases, setDatabases] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);
  const [lastRefreshed, setLastRefreshed] = useState<Date | null>(null);

  const status = activeProfileId ? statuses[activeProfileId] : null;
  const isConnected = status?.state === 'connected' || status?.state === 'authenticated';

  const refresh = useCallback(async () => {
    if (!isConnected) return;
    setLoading(true);
    try {
      const [pingRes, capRes, dbRes] = await Promise.allSettled([
        request<Record<string, unknown>>({ action: 'ping' }),
        request<Record<string, unknown>>({ action: 'community_capabilities' }),
        request<{ databases?: string[] }>({ action: 'listDatabases' }),
      ]);

      if (pingRes.status === 'fulfilled') setPing(pingRes.value.response);
      if (capRes.status === 'fulfilled') setCapabilities(capRes.value.response);
      if (dbRes.status === 'fulfilled') setDatabases(dbRes.value.response.databases ?? []);
      setLastRefreshed(new Date());
    } catch (e) {
      notify({ type: 'error', title: 'Failed to refresh overview', message: String(e) });
    } finally {
      setLoading(false);
    }
  }, [isConnected, request]);

  useEffect(() => { refresh(); }, [isConnected]);

  if (!isConnected) {
    return (
      <div className="h-full flex items-center justify-center">
        <div className="text-center">
          <XCircle size={40} className="text-surface-600 mx-auto mb-3" />
          <p className="text-md font-semibold text-surface-300">Not connected</p>
          <p className="text-sm text-surface-500 mt-1">Connect to a PacificDB server to view overview.</p>
          <Button variant="primary" size="sm" className="mt-4" onClick={() => navigateTo('welcome' as never)}>
            Manage Connections
          </Button>
        </div>
      </div>
    );
  }

  if (loading && !ping) return <SkeletonBlock rows={8} className="p-6" />;

  const cap = capabilities as {
    community?: boolean;
    vectors?: boolean;
    media?: boolean;
    backups?: boolean;
    security?: boolean;
    raft?: boolean;
    [k: string]: unknown;
  } | null;

  const serverVersion = (ping as { version?: string })?.version ?? 'Community';
  const nodeRole = (ping as { isLeader?: boolean })?.isLeader === true ? 'Leader' : 'Follower';
  const raftTerm = (ping as { term?: number })?.term;

  const features: [string, boolean][] = [
    ['Documents',  true],
    ['Vectors',    cap?.vectors !== false],
    ['Media',      cap?.media !== false],
    ['Backups',    cap?.backups !== false],
    ['Security',   cap?.security !== false],
    ['Raft / HA',  cap?.raft !== false],
  ];

  return (
    <div className="h-full overflow-auto p-6 space-y-6">
      {/* Header */}
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-xl font-bold text-surface-50">Server Overview</h1>
          {lastRefreshed && (
            <p className="text-xs text-surface-500 mt-0.5">
              Last refreshed {lastRefreshed.toLocaleTimeString()}
            </p>
          )}
        </div>
        <Button
          variant="ghost"
          size="sm"
          icon={<RefreshCw size={12} className={loading ? 'animate-spin' : ''} />}
          onClick={refresh}
          loading={loading}
          id="refresh-overview-btn"
        >
          Refresh
        </Button>
      </div>

      {/* Stat cards */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
        <StatCard
          label="Status"
          value={status?.state ?? 'unknown'}
          sub={status?.authenticatedAs ? `as ${status.authenticatedAs}` : undefined}
          icon={<Wifi size={16} />}
          accent="text-success"
        />
        <StatCard
          label="Version"
          value={String(serverVersion)}
          sub="PacificDB Community"
          icon={<Server size={16} />}
        />
        <StatCard
          label="Databases"
          value={databases.length}
          sub="total"
          icon={<Database size={16} />}
          accent="text-purple-400"
        />
        <StatCard
          label="Node Role"
          value={nodeRole}
          sub={raftTerm !== undefined ? `Term ${raftTerm}` : undefined}
          icon={<Activity size={16} />}
          accent="text-warning"
        />
      </div>

      {/* Capabilities */}
      {cap && (
        <div className="card">
          <h2 className="text-sm font-semibold text-surface-100 mb-3 flex items-center gap-2">
            <Cpu size={14} className="text-pacific-400" />
            Server Capabilities
          </h2>
          <div className="grid grid-cols-2 sm:grid-cols-3 gap-2">
            {features.map(([label, enabled]) => (
              <div key={label} className="flex items-center gap-2 text-sm">
                {enabled
                  ? <CheckCircle2 size={13} className="text-success flex-shrink-0" />
                  : <XCircle size={13} className="text-surface-600 flex-shrink-0" />}
                <span className={enabled ? 'text-surface-200' : 'text-surface-500'}>{label}</span>
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Databases */}
      <div className="card">
        <h2 className="text-sm font-semibold text-surface-100 mb-3 flex items-center gap-2">
          <Database size={14} className="text-pacific-400" />
          Databases ({databases.length})
        </h2>
        {databases.length === 0 ? (
          <p className="text-sm text-surface-500">No databases. Create one from the sidebar.</p>
        ) : (
          <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 gap-2">
            {databases.map(db => (
              <button
                key={db}
                onClick={() => navigateTo('data', db)}
                className="flex items-center gap-2 px-3 py-2 bg-surface-700 hover:bg-surface-600 rounded-md text-sm text-surface-200 hover:text-surface-50 transition-colors text-left"
                id={`db-${db}`}
              >
                <Database size={12} className="text-pacific-400 flex-shrink-0" />
                <span className="truncate font-mono">{db}</span>
              </button>
            ))}
          </div>
        )}
      </div>

      {/* Raw ping response */}
      {ping && (
        <div className="card">
          <h2 className="text-sm font-semibold text-surface-100 mb-2 flex items-center gap-2">
            <Shield size={14} className="text-surface-400" />
            Raw Server Response (ping)
          </h2>
          <pre className="text-xs text-surface-300 bg-surface-900 rounded p-3 overflow-auto selectable font-mono max-h-40">
            {JSON.stringify(ping, null, 2)}
          </pre>
        </div>
      )}
    </div>
  );
}
