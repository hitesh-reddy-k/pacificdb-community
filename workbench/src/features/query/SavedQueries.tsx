import { useState, useEffect, useCallback } from 'react';
import { Bookmark, Plus, Trash2, Play, Search as SearchIcon } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Input } from '../../components/ui/Input';
import { EmptyState } from '../../components/ui/EmptyState';
import { clsx } from 'clsx';

interface SavedQuery {
  id: string;
  name: string;
  description?: string;
  tags: string[];
  action: string;
  request: Record<string, unknown>;
  createdAt: string;
}

export function SavedQueries() {
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();
  const [queries, setQueries] = useState<SavedQuery[]>([]);
  const [search, setSearch] = useState('');
  const [selected, setSelected] = useState<SavedQuery | null>(null);
  const [running, setRunning] = useState(false);
  const [result, setResult] = useState<unknown>(null);

  const load = useCallback(async () => {
    try {
      const q = await window.pacific.queries.list() as SavedQuery[];
      setQueries(q);
    } catch { setQueries([]); }
  }, []);

  useEffect(() => { load(); }, []);

  const deleteQuery = async (id: string) => {
    await window.pacific.queries.delete(id);
    setQueries(q => q.filter(x => x.id !== id));
    if (selected?.id === id) setSelected(null);
  };

  const runQuery = async (q: SavedQuery) => {
    setRunning(true);
    setResult(null);
    try {
      const { response } = await request(q.request);
      setResult(response);
      notify({ type: 'success', title: 'Query executed' });
    } catch (e) {
      notify({ type: 'error', title: 'Query failed', message: String(e) });
    } finally {
      setRunning(false);
    }
  };

  const filtered = queries.filter(q =>
    q.name.toLowerCase().includes(search.toLowerCase()) ||
    q.action.toLowerCase().includes(search.toLowerCase()) ||
    q.tags.some(t => t.toLowerCase().includes(search.toLowerCase()))
  );

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <Bookmark size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Saved Queries</span>
        <div className="flex-1" />
      </div>

      <div className="flex-1 flex overflow-hidden">
        {/* List */}
        <div className="w-72 flex-shrink-0 border-r border-surface-500/30 flex flex-col">
          <div className="p-2 border-b border-surface-500/30">
            <div className="relative">
              <SearchIcon size={11} className="absolute left-2.5 top-1/2 -translate-y-1/2 text-surface-500" />
              <input
                value={search}
                onChange={e => setSearch(e.target.value)}
                placeholder="Search queries…"
                className="input pl-7 text-xs py-1.5"
                id="saved-query-search"
              />
            </div>
          </div>
          <div className="flex-1 overflow-auto">
            {filtered.length === 0 ? (
              <EmptyState icon={<Bookmark size={14} />} title="No saved queries" description="Save queries from the Query Workbench." compact className="py-8" />
            ) : filtered.map(q => (
              <div
                key={q.id}
                onClick={() => setSelected(q)}
                className={clsx(
                  'px-3 py-2.5 cursor-pointer border-b border-surface-500/20 hover:bg-surface-700/30 transition-colors group',
                  selected?.id === q.id && 'bg-pacific-500/10',
                )}
              >
                <div className="flex items-center gap-2">
                  <p className="text-xs font-semibold text-surface-100 flex-1 truncate">{q.name}</p>
                </div>
                <p className="text-2xs text-surface-500 font-mono mt-0.5 truncate">{q.action}</p>
                {q.tags.length > 0 && (
                  <div className="flex gap-1 mt-1.5 flex-wrap">
                    {q.tags.map(t => <span key={t} className="tag text-2xs">{t}</span>)}
                  </div>
                )}
              </div>
            ))}
          </div>
        </div>

        {/* Detail */}
        {selected ? (
          <div className="flex-1 flex flex-col overflow-hidden">
            <div className="flex items-center gap-2 px-4 py-2 border-b border-surface-500/30">
              <span className="text-sm font-semibold text-surface-100">{selected.name}</span>
              <div className="flex-1" />
              <Button variant="primary" size="xs" icon={<Play size={11} />} loading={running} onClick={() => runQuery(selected)} id="run-saved-query-btn">Run</Button>
              <Button variant="danger" size="xs" icon={<Trash2 size={11} />} onClick={() => deleteQuery(selected.id)}>Delete</Button>
            </div>
            <div className="flex-1 overflow-auto p-4 space-y-4">
              {selected.description && (
                <p className="text-xs text-surface-400">{selected.description}</p>
              )}
              <div>
                <p className="text-2xs text-surface-500 uppercase tracking-wide mb-1.5">Request</p>
                <pre className="text-xs font-mono text-surface-200 selectable whitespace-pre-wrap bg-surface-800 border border-surface-500/30 rounded p-3">
                  {JSON.stringify(selected.request, null, 2)}
                </pre>
              </div>
              {result !== null && (
                <div>
                  <p className="text-2xs text-surface-500 uppercase tracking-wide mb-1.5">Last Result</p>
                  <pre className="text-xs font-mono text-surface-200 selectable whitespace-pre-wrap bg-surface-800 border border-surface-500/30 rounded p-3">
                    {JSON.stringify(result, null, 2)}
                  </pre>
                </div>
              )}
            </div>
          </div>
        ) : (
          <div className="flex-1 flex items-center justify-center text-surface-600">
            <p className="text-sm">Select a saved query</p>
          </div>
        )}
      </div>
    </div>
  );
}
