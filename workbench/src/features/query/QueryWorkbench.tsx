import { useState, useCallback } from 'react';
import {
  Play, Zap, History, ChevronDown, Clock, AlertCircle,
  CheckCircle2, Copy, Download, Columns2,
} from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Tabs, TabPanel, useTabs } from '../../components/ui/Tabs';
import { JsonEditor } from '../../components/ui/JsonEditor';
import { EmptyState } from '../../components/ui/EmptyState';
import { clsx } from 'clsx';

type ResultView = 'table' | 'json' | 'raw';

const EXAMPLE_QUERIES: { label: string; json: object }[] = [
  { label: 'Find all', json: { action: 'find', collection: 'YOUR_COLLECTION', filter: {}, limit: 50 } },
  { label: 'Count', json: { action: 'count', collection: 'YOUR_COLLECTION', filter: {} } },
  { label: 'Find with filter', json: { action: 'find', collection: 'YOUR_COLLECTION', filter: { field: 'value' }, limit: 10 } },
  { label: 'Aggregate', json: { action: 'aggregate', collection: 'YOUR_COLLECTION', pipeline: [{ '$match': {} }, { '$limit': 10 }] } },
  { label: 'Explain', json: { action: 'explain', collection: 'YOUR_COLLECTION', filter: {} } },
  { label: 'List collections', json: { action: 'listCollections' } },
  { label: 'Ping', json: { action: 'ping' } },
];

interface QueryResult {
  response: unknown;
  durationMs: number;
  executedAt: Date;
  error?: string;
  request: unknown;
}

function ResultTable({ data }: { data: Record<string, unknown>[] }) {
  if (!data.length) return <p className="text-xs text-surface-500 p-4">No results</p>;
  const cols = Object.keys(data[0]);
  return (
    <table className="w-full text-xs border-collapse">
      <thead className="sticky top-0 bg-surface-800 border-b border-surface-500/30">
        <tr>
          {cols.map(c => (
            <th key={c} className="px-3 py-2 text-left font-medium text-surface-300 whitespace-nowrap border-r border-surface-500/20 last:border-0">
              {c}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {data.map((row, i) => (
          <tr key={i} className="border-b border-surface-500/20 hover:bg-surface-700/20">
            {cols.map(c => (
              <td key={c} className="px-3 py-1.5 font-mono text-surface-200 max-w-xs truncate border-r border-surface-500/10 last:border-0">
                {JSON.stringify(row[c])}
              </td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function QueryWorkbench() {
  const activeDatabase = useWorkspaceStore(s => s.activeDatabase);
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();
  const { activeTab, setActiveTab } = useTabs('editor');

  const defaultQuery = JSON.stringify(
    activeCollection
      ? { action: 'find', collection: activeCollection, filter: {}, limit: 50 }
      : { action: 'ping' },
    null, 2,
  );

  const [query, setQuery] = useState(defaultQuery);
  const [result, setResult] = useState<QueryResult | null>(null);
  const [running, setRunning] = useState(false);
  const [resultView, setResultView] = useState<ResultView>('json');
  const [showInspector, setShowInspector] = useState(false);
  const [showExamples, setShowExamples] = useState(false);

  const run = useCallback(async () => {
    let parsed: Record<string, unknown>;
    try { parsed = JSON.parse(query); }
    catch (e) {
      notify({ type: 'error', title: 'Invalid JSON', message: String(e) });
      return;
    }

    setRunning(true);
    setResult(null);
    try {
      const { response, durationMs } = await request(parsed);
      setResult({ response, durationMs, executedAt: new Date(), request: parsed });
      setActiveTab('results');
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      setResult({ response: null, durationMs: 0, executedAt: new Date(), error: msg, request: parsed });
      setActiveTab('results');
    } finally {
      setRunning(false);
    }
  }, [query, request]);

  const explain = useCallback(async () => {
    let parsed: Record<string, unknown>;
    try { parsed = JSON.parse(query); }
    catch { return; }
    if (!parsed.collection) { notify({ type: 'warning', title: 'Explain requires a collection' }); return; }
    setRunning(true);
    try {
      const { response, durationMs } = await request({
        action: 'explain',
        collection: parsed.collection,
        filter: (parsed.filter ?? {}) as Record<string, unknown>,
      });
      setResult({ response, durationMs, executedAt: new Date(), request: parsed });
      setActiveTab('results');
    } catch (e) {
      setResult({ response: null, durationMs: 0, executedAt: new Date(), error: String(e), request: parsed });
    } finally {
      setRunning(false);
    }
  }, [query, request]);

  const copyResult = () => {
    if (!result) return;
    navigator.clipboard.writeText(JSON.stringify(result.response, null, 2));
    notify({ type: 'success', title: 'Copied', duration: 2000 });
  };

  const exportResult = () => {
    if (!result) return;
    const blob = new Blob([JSON.stringify(result.response, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `query-result-${Date.now()}.json`;
    a.click();
  };

  const resultData: Record<string, unknown>[] | null = (() => {
    if (!result?.response) return null;
    const r = result.response as Record<string, unknown>;
    if (Array.isArray(r.data)) return r.data as Record<string, unknown>[];
    if (Array.isArray(r.results)) return r.results as Record<string, unknown>[];
    return null;
  })();

  const TABS = [
    { id: 'editor', label: 'Query Editor' },
    { id: 'results', label: 'Results', badge: result ? (result.error ? '!' : '✓') : undefined },
    { id: 'inspector', label: 'Inspector' },
  ];

  return (
    <div className="h-full flex flex-col">
      {/* Context bar */}
      <div className="flex items-center gap-2 px-4 py-2 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <span className="text-xs text-surface-500">Context:</span>
        <span className="font-mono text-xs text-pacific-300">{activeDatabase ?? '—'}</span>
        {activeCollection && (
          <>
            <span className="text-surface-600">›</span>
            <span className="font-mono text-xs text-surface-200">{activeCollection}</span>
          </>
        )}
        <div className="flex-1" />

        {/* Examples dropdown */}
        <div className="relative">
          <Button variant="ghost" size="xs" icon={<ChevronDown size={11} />} onClick={() => setShowExamples(v => !v)}>
            Examples
          </Button>
          {showExamples && (
            <>
              <div className="fixed inset-0 z-40" onClick={() => setShowExamples(false)} />
              <div className="absolute right-0 top-full mt-1 z-50 bg-surface-700 border border-surface-500/40 rounded-lg shadow-menu min-w-52 py-1 animate-fade-in">
                {EXAMPLE_QUERIES.map(ex => (
                  <button
                    key={ex.label}
                    onClick={() => { setQuery(JSON.stringify(ex.json, null, 2)); setShowExamples(false); }}
                    className="w-full text-left px-3 py-1.5 text-xs text-surface-200 hover:bg-surface-600 transition-colors"
                  >
                    {ex.label}
                  </button>
                ))}
              </div>
            </>
          )}
        </div>

        <Button variant="ghost" size="xs" icon={<Columns2 size={11} />} onClick={() => setShowInspector(v => !v)}>
          Inspector
        </Button>
        <Button variant="ghost" size="xs" icon={<Zap size={11} />} onClick={explain} loading={running}>
          Explain
        </Button>
        <Button variant="primary" size="sm" icon={<Play size={12} />} onClick={run} loading={running} id="run-query-btn">
          Run
        </Button>
      </div>

      <div className="flex-1 overflow-hidden flex flex-col">
        <Tabs tabs={TABS} activeTab={activeTab} onChange={setActiveTab}>
          {/* Editor */}
          <TabPanel id="editor" activeTab={activeTab} className="p-4 h-full">
            <JsonEditor value={query} onChange={setQuery} placeholder="Enter a PacificDB JSON request…" />
          </TabPanel>

          {/* Results */}
          <TabPanel id="results" activeTab={activeTab} className="flex flex-col h-full">
            {!result ? (
              <EmptyState
                icon={<Play size={18} />}
                title="No results yet"
                description="Run a query to see results here."
                compact
                className="h-full"
              />
            ) : (
              <>
                {/* Results toolbar */}
                <div className="flex items-center gap-2 px-4 py-2 border-b border-surface-500/30 flex-shrink-0">
                  <div className="flex items-center gap-1.5 text-xs">
                    {result.error
                      ? <><AlertCircle size={12} className="text-danger" /><span className="text-danger">Error</span></>
                      : <><CheckCircle2 size={12} className="text-success" /><span className="text-success">Success</span></>}
                    <span className="text-surface-500">·</span>
                    <Clock size={11} className="text-surface-500" />
                    <span className="text-surface-400">{result.durationMs}ms</span>
                    {resultData && (
                      <>
                        <span className="text-surface-500">·</span>
                        <span className="text-surface-400">{resultData.length} rows</span>
                      </>
                    )}
                  </div>
                  <div className="flex-1" />
                  {/* Result view selector */}
                  {resultData && (
                    <div className="flex bg-surface-700 rounded p-0.5 gap-0.5">
                      {(['table', 'json'] as ResultView[]).map(m => (
                        <button
                          key={m}
                          onClick={() => setResultView(m)}
                          className={clsx(
                            'px-2 py-0.5 rounded text-2xs font-medium transition-colors',
                            resultView === m ? 'bg-surface-500 text-surface-50' : 'text-surface-400 hover:text-surface-200',
                          )}
                        >{m}</button>
                      ))}
                    </div>
                  )}
                  <Button variant="ghost" size="xs" icon={<Copy size={11} />} onClick={copyResult}>Copy</Button>
                  <Button variant="ghost" size="xs" icon={<Download size={11} />} onClick={exportResult}>Export</Button>
                </div>

                {/* Results display */}
                <div className="flex-1 overflow-auto">
                  {result.error ? (
                    <div className="p-4">
                      <div className="bg-danger/10 border border-danger/30 rounded-lg p-4">
                        <p className="text-xs font-semibold text-danger mb-1">Query Error</p>
                        <p className="text-xs text-danger/80 font-mono selectable">{result.error}</p>
                      </div>
                    </div>
                  ) : resultData && resultView === 'table' ? (
                    <ResultTable data={resultData} />
                  ) : (
                    <pre className="p-4 text-xs text-surface-200 font-mono selectable whitespace-pre-wrap">
                      {JSON.stringify(result.response, null, 2)}
                    </pre>
                  )}
                </div>
              </>
            )}
          </TabPanel>

          {/* Inspector */}
          <TabPanel id="inspector" activeTab={activeTab} className="flex h-full divide-x divide-surface-500/30">
            <div className="flex-1 p-4 overflow-auto">
              <p className="text-2xs text-surface-500 uppercase tracking-wide mb-2">Sent Request</p>
              <pre className="text-xs text-surface-300 font-mono selectable whitespace-pre-wrap">
                {result ? JSON.stringify(result.request, null, 2) : '—'}
              </pre>
            </div>
            <div className="flex-1 p-4 overflow-auto">
              <p className="text-2xs text-surface-500 uppercase tracking-wide mb-2">
                Response {result?.durationMs != null ? `(${result.durationMs}ms)` : ''}
              </p>
              <pre className="text-xs text-surface-300 font-mono selectable whitespace-pre-wrap">
                {result ? JSON.stringify(result.response ?? result.error, null, 2) : '—'}
              </pre>
            </div>
          </TabPanel>
        </Tabs>
      </div>
    </div>
  );
}
