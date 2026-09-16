import { useEffect, useState } from 'react';
import { Activity, RefreshCw } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';

export function MetricsDashboard() {
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const { request } = usePacific();
  const [metrics, setMetrics] = useState<Record<string, unknown> | null>(null);
  const [loading, setLoading] = useState(false);
  const [autoRefresh, setAutoRefresh] = useState(false);

  const refresh = async () => {
    setLoading(true);
    try {
      const { response } = await request<Record<string, unknown>>({ action: 'metrics' });
      setMetrics(response);
    } catch { /* metrics may not be available */ }
    finally { setLoading(false); }
  };

  useEffect(() => { refresh(); }, [activeProfileId]);
  useEffect(() => {
    if (!autoRefresh) return;
    const interval = setInterval(refresh, 5000);
    return () => clearInterval(interval);
  }, [autoRefresh]);

  return (
    <div className="h-full overflow-auto p-6 space-y-4">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <Activity size={18} className="text-pacific-400" />
          <h1 className="text-lg font-bold text-surface-50">Metrics</h1>
        </div>
        <div className="flex items-center gap-2">
          <label className="flex items-center gap-1.5 text-xs text-surface-400 cursor-pointer">
            <input type="checkbox" checked={autoRefresh} onChange={e => setAutoRefresh(e.target.checked)} className="accent-pacific-500" />
            Auto-refresh (5s)
          </label>
          <Button variant="ghost" size="xs" icon={<RefreshCw size={11} className={loading ? 'animate-spin' : ''} />} onClick={refresh} loading={loading}>Refresh</Button>
        </div>
      </div>
      <div className="card">
        <pre className="text-xs text-surface-300 font-mono selectable whitespace-pre-wrap overflow-auto">
          {metrics ? JSON.stringify(metrics, null, 2) : 'Metrics not available from this server.'}
        </pre>
      </div>
    </div>
  );
}
