import { useState, useEffect, useCallback } from 'react';
import { Network, RefreshCw, Activity } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { SkeletonBlock } from '../../components/ui/EmptyState';

export function ClusterDashboard() {
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const { request } = usePacific();

  const [pingData, setPingData] = useState<Record<string, unknown> | null>(null);
  const [loading, setLoading] = useState(false);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const { response } = await request<Record<string, unknown>>({ action: 'ping' });
      setPingData(response);
    } catch { setPingData(null); }
    finally { setLoading(false); }
  }, [request]);

  useEffect(() => { refresh(); }, [activeProfileId]);

  const isLeader = pingData?.isLeader === true;
  const term = pingData?.term;
  const nodeId = pingData?.node_id ?? pingData?.nodeId;
  const commitIndex = pingData?.commit_index ?? pingData?.commitIndex;
  const lastApplied = pingData?.last_applied ?? pingData?.lastApplied;

  return (
    <div className="h-full overflow-auto p-6 space-y-6">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <Network size={18} className="text-pacific-400" />
          <h1 className="text-lg font-bold text-surface-50">Cluster</h1>
        </div>
        <Button variant="ghost" size="xs" icon={<RefreshCw size={11} className={loading ? 'animate-spin' : ''} />} onClick={refresh} loading={loading}>Refresh</Button>
      </div>

      {loading && !pingData ? <SkeletonBlock rows={4} /> : (
        <div className="grid grid-cols-2 gap-4">
          <div className="card">
            <p className="section-header mb-3">Node Status</p>
            <div className="space-y-2.5">
              {[
                ['Node ID', nodeId ?? '—'],
                ['Role', isLeader ? '🟢 Leader' : '⚪ Follower'],
                ['Term', String(term ?? '—')],
                ['Commit Index', String(commitIndex ?? '—')],
                ['Last Applied', String(lastApplied ?? '—')],
              ].map(([label, value]) => (
                <div key={label} className="flex items-center justify-between">
                  <span className="text-xs text-surface-400">{label}</span>
                  <span className="text-xs font-mono text-surface-100">{value}</span>
                </div>
              ))}
            </div>
          </div>

          <div className="card">
            <p className="section-header mb-3">Raft Details</p>
            <pre className="text-xs text-surface-300 font-mono selectable whitespace-pre-wrap">
              {pingData ? JSON.stringify(pingData, null, 2) : '—'}
            </pre>
          </div>
        </div>
      )}
    </div>
  );
}
