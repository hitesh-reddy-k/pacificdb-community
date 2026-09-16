import { useEffect, useState, useCallback } from 'react';
import { History, Trash2, Play, RefreshCw } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { EmptyState } from '../../components/ui/EmptyState';
import { clsx } from 'clsx';

interface HistoryEntry {
  id: string;
  action: string;
  database?: string;
  collection?: string;
  durationMs: number;
  success: boolean;
  error?: string;
  executedAt: string;
  request: Record<string, unknown>;
}

export function QueryHistory() {
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();

  const [history, setHistory] = useState<HistoryEntry[]>([]);
  const [loading, setLoading] = useState(false);
  const [selected, setSelected] = useState<HistoryEntry | null>(null);

  const load = useCallback(async () => {
    if (!activeProfileId) return;
    setLoading(true);
    try {
      const entries = await window.pacific.history.list(activeProfileId) as HistoryEntry[];
      setHistory(entries);
    } catch { setHistory([]); }
    finally { setLoading(false); }
  }, [activeProfileId]);

  useEffect(() => { load(); }, [activeProfileId]);

  const deleteEntry = async (id: string) => {
    await window.pacific.history.delete(activeProfileId!, id);
    setHistory(h => h.filter(e => e.id !== id));
    if (selected?.id === id) setSelected(null);
  };

  const rerun = async (entry: HistoryEntry) => {
    try {
      const { response } = await request(entry.request);
      notify({ type: 'success', title: 'Query re-executed' });
    } catch (e) {
      notify({ type: 'error', title: 'Re-run failed', message: String(e) });
    }
  };

  // Group by date
  const grouped: Record<string, HistoryEntry[]> = {};
  for (const e of history) {
    const date = new Date(e.executedAt).toLocaleDateString();
    (grouped[date] ??= []).push(e);
  }

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <History size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Query History</span>
        <div className="flex-1" />
        <Button variant="ghost" size="xs" icon={<RefreshCw size={11} />} onClick={load} loading={loading}>Refresh</Button>
        {history.length > 0 && (
          <Button variant="ghost" size="xs" icon={<Trash2 size={11} />} onClick={async () => {
            await window.pacific.history.clear(activeProfileId!);
            setHistory([]); setSelected(null);
          }}>Clear All</Button>
        )}
      </div>

      <div className="flex-1 flex overflow-hidden">
        {/* List */}
        <div className="w-80 flex-shrink-0 border-r border-surface-500/30 overflow-auto">
          {history.length === 0 ? (
            <EmptyState icon={<History size={16} />} title="No history" description="Executed queries will appear here." compact className="py-12" />
          ) : (
            Object.entries(grouped).map(([date, entries]) => (
              <div key={date}>
                <p className="section-header sticky top-0 bg-surface-800 py-1.5">{date}</p>
                {entries.map(e => (
                  <div
                    key={e.id}
                    onClick={() => setSelected(e)}
                    className={clsx(
                      'px-3 py-2 cursor-pointer border-b border-surface-500/20 hover:bg-surface-700/30 transition-colors group',
                      selected?.id === e.id && 'bg-pacific-500/10',
                    )}
                  >
                    <div className="flex items-center gap-2">
                      <div className={clsx('w-1.5 h-1.5 rounded-full flex-shrink-0', e.success ? 'bg-success' : 'bg-danger')} />
                      <span className="font-mono text-xs font-semibold text-surface-200 truncate flex-1">{e.action}</span>
                      <span className="text-2xs text-surface-500 flex-shrink-0">{e.durationMs}ms</span>
                    </div>
                    <p className="text-2xs text-surface-500 mt-0.5 pl-3.5 truncate">
                      {new Date(e.executedAt).toLocaleTimeString()}
                      {e.collection && ` · ${e.collection}`}
                    </p>
                  </div>
                ))}
              </div>
            ))
          )}
        </div>

        {/* Detail */}
        {selected ? (
          <div className="flex-1 flex flex-col overflow-hidden">
            <div className="flex items-center gap-2 px-4 py-2 border-b border-surface-500/30">
              <span className={clsx('w-2 h-2 rounded-full', selected.success ? 'bg-success' : 'bg-danger')} />
              <span className="font-mono text-xs font-semibold text-surface-200">{selected.action}</span>
              <span className="text-2xs text-surface-500">{selected.durationMs}ms</span>
              <div className="flex-1" />
              <Button variant="ghost" size="xs" icon={<Play size={11} />} onClick={() => rerun(selected)}>Re-run</Button>
              <Button variant="ghost" size="xs" icon={<Trash2 size={11} />} onClick={() => deleteEntry(selected.id)}>Delete</Button>
            </div>
            <div className="flex-1 overflow-auto p-4 space-y-3">
              <div>
                <p className="text-2xs text-surface-500 uppercase tracking-wide mb-1.5">Request</p>
                <pre className="text-xs font-mono text-surface-200 selectable whitespace-pre-wrap bg-surface-800 border border-surface-500/30 rounded p-3">
                  {JSON.stringify(selected.request, null, 2)}
                </pre>
              </div>
              {selected.error && (
                <div>
                  <p className="text-2xs text-danger uppercase tracking-wide mb-1.5">Error</p>
                  <p className="text-xs font-mono text-danger selectable">{selected.error}</p>
                </div>
              )}
            </div>
          </div>
        ) : (
          <div className="flex-1 flex items-center justify-center text-surface-600">
            <p className="text-sm">Select an entry to view details</p>
          </div>
        )}
      </div>
    </div>
  );
}
