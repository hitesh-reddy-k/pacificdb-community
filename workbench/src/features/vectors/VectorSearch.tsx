import { useState, useCallback } from 'react';
import { Brain, Search, Plus, AlertCircle } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Input, Select } from '../../components/ui/Input';
import { EmptyState } from '../../components/ui/EmptyState';

interface VectorResult {
  id: string;
  score: number;
  document: Record<string, unknown>;
}

export function VectorSearch() {
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();

  const [vectorInput, setVectorInput] = useState('[0.1, 0.2, 0.3]');
  const [k, setK] = useState('10');
  const [metric, setMetric] = useState('cosine');
  const [collection, setCollection] = useState(activeCollection ?? '');
  const [filter, setFilter] = useState('{}');
  const [results, setResults] = useState<VectorResult[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  // Insert vector form
  const [insertId, setInsertId] = useState('');
  const [insertVector, setInsertVector] = useState('[0.1, 0.2, 0.3]');
  const [insertMeta, setInsertMeta] = useState('{}');
  const [inserting, setInserting] = useState(false);

  const handleSearch = useCallback(async () => {
    setError('');
    let vector: number[];
    try { vector = JSON.parse(vectorInput); }
    catch { setError('Invalid vector JSON. Use format: [0.1, 0.2, 0.3]'); return; }
    if (!Array.isArray(vector) || vector.some(v => typeof v !== 'number')) {
      setError('Vector must be an array of numbers');
      return;
    }
    let parsedFilter: Record<string, unknown> = {};
    try { parsedFilter = JSON.parse(filter); } catch { /* ignore */ }

    if (!collection) { setError('Select or enter a collection name'); return; }

    setLoading(true);
    try {
      const { response } = await request<{ results?: VectorResult[] }>({
        action: 'queryVector',
        collection,
        vector,
        k: Number(k),
        metric,
        filter: parsedFilter,
      });
      setResults(response.results ?? []);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }, [vectorInput, k, metric, collection, filter, request]);

  const handleInsert = async () => {
    let vector: number[];
    let meta: Record<string, unknown> = {};
    try { vector = JSON.parse(insertVector); } catch { notify({ type: 'error', title: 'Invalid vector JSON' }); return; }
    try { meta = JSON.parse(insertMeta); } catch { /* ignore */ }
    if (!insertId.trim()) { notify({ type: 'error', title: 'Vector ID is required' }); return; }
    if (!collection) { notify({ type: 'error', title: 'Collection is required' }); return; }

    setInserting(true);
    try {
      await request({
        action: 'insertVector',
        collection,
        data: { ...meta, id: insertId.trim(), kind: 'vector', vector },
      });
      notify({ type: 'success', title: 'Vector inserted' });
      setInsertId('');
    } catch (e) {
      notify({ type: 'error', title: 'Insert failed', message: String(e) });
    } finally {
      setInserting(false);
    }
  };

  return (
    <div className="h-full flex flex-col overflow-auto p-6 gap-6">
      <div className="flex items-center gap-2">
        <Brain size={18} className="text-purple-400" />
        <h1 className="text-lg font-bold text-surface-50">Vector Search</h1>
      </div>

      <div className="grid grid-cols-3 gap-6">
        {/* Search form */}
        <div className="col-span-2 card flex flex-col gap-4">
          <h2 className="text-sm font-semibold text-surface-200">Query Vectors</h2>
          <div className="grid grid-cols-2 gap-3">
            <Input
              label="Collection"
              id="vec-collection"
              value={collection}
              onChange={e => setCollection(e.target.value)}
              placeholder="my_vectors"
              className="font-mono"
            />
            <Select label="Metric" id="vec-metric" value={metric} onChange={e => setMetric(e.target.value)}>
              <option value="cosine">Cosine</option>
              <option value="euclidean">Euclidean</option>
              <option value="dot">Dot Product</option>
            </Select>
          </div>
          <Input
            label="Top K results"
            id="vec-k"
            type="number"
            value={k}
            onChange={e => setK(e.target.value)}
          />
          <div>
            <label className="text-xs font-medium text-surface-200 block mb-1">Query Vector (JSON array)</label>
            <textarea
              id="vec-vector-input"
              value={vectorInput}
              onChange={e => setVectorInput(e.target.value)}
              className="input font-mono h-20 resize-none selectable"
              placeholder="[0.1, 0.2, 0.3, ...]"
            />
          </div>
          <div>
            <label className="text-xs font-medium text-surface-200 block mb-1">Filter (optional JSON)</label>
            <textarea
              id="vec-filter-input"
              value={filter}
              onChange={e => setFilter(e.target.value)}
              className="input font-mono h-16 resize-none selectable"
              placeholder='{}'
            />
          </div>
          {error && (
            <div className="flex items-start gap-2 text-xs text-danger">
              <AlertCircle size={12} className="flex-shrink-0 mt-0.5" />{error}
            </div>
          )}
          <Button variant="primary" icon={<Search size={13} />} onClick={handleSearch} loading={loading} id="vector-search-btn">
            Search Vectors
          </Button>
        </div>

        {/* Insert form */}
        <div className="card flex flex-col gap-3">
          <h2 className="text-sm font-semibold text-surface-200 flex items-center gap-2">
            <Plus size={13} /> Insert Vector
          </h2>
          <Input label="Vector ID" id="vec-insert-id" value={insertId} onChange={e => setInsertId(e.target.value)} placeholder="my-vector-id" className="font-mono" />
          <div>
            <label className="text-xs font-medium text-surface-200 block mb-1">Vector (JSON array)</label>
            <textarea
              id="vec-insert-vector"
              value={insertVector}
              onChange={e => setInsertVector(e.target.value)}
              className="input font-mono h-24 resize-none selectable"
              placeholder="[0.1, 0.2, ...]"
            />
          </div>
          <div>
            <label className="text-xs font-medium text-surface-200 block mb-1">Metadata (JSON)</label>
            <textarea
              id="vec-insert-meta"
              value={insertMeta}
              onChange={e => setInsertMeta(e.target.value)}
              className="input font-mono h-16 resize-none selectable"
              placeholder='{}'
            />
          </div>
          <Button variant="outline" icon={<Plus size={12} />} size="sm" onClick={handleInsert} loading={inserting} id="insert-vector-btn">
            Insert
          </Button>
        </div>
      </div>

      {/* Results */}
      {results.length > 0 && (
        <div className="card">
          <h2 className="text-sm font-semibold text-surface-200 mb-3">
            Results ({results.length} vectors)
          </h2>
          <div className="space-y-2">
            {results.map((r, i) => (
              <div key={r.id ?? i} className="flex items-start gap-3 p-3 bg-surface-700/50 rounded-lg">
                <div className="w-8 h-8 rounded-full bg-surface-600 flex items-center justify-center text-2xs font-bold text-surface-300 flex-shrink-0">
                  {i + 1}
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2">
                    <p className="font-mono text-xs font-semibold text-surface-100">{r.id}</p>
                    <span className="badge-purple">score: {typeof r.score === 'number' ? r.score.toFixed(4) : r.score}</span>
                  </div>
                  <pre className="text-2xs text-surface-400 font-mono mt-1 truncate">
                    {JSON.stringify(r.document)}
                  </pre>
                </div>
              </div>
            ))}
          </div>
        </div>
      )}

      {!loading && results.length === 0 && (
        <EmptyState icon={<Brain size={18} />} title="No results" description="Run a vector search to see similar vectors." compact />
      )}
    </div>
  );
}
