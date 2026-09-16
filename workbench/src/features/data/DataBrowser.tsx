import { useState, useEffect, useCallback, useRef } from 'react';
import {
  RefreshCw, Plus, Trash2, Edit2, Copy, Download, ChevronLeft, ChevronRight,
  LayoutGrid, Code, AlignLeft, Filter, SortAsc, SortDesc, Search,
} from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Input } from '../../components/ui/Input';
import { EmptyState, SkeletonBlock } from '../../components/ui/EmptyState';
import { ConfirmDialog } from '../../components/ui/Dialog';
import { InsertDocumentDialog } from './InsertDocumentDialog';
import { DocumentViewer } from './DocumentViewer';
import { clsx } from 'clsx';

type ViewMode = 'table' | 'json' | 'raw';

const PAGE_SIZE_OPTIONS = [25, 50, 100, 200];
const INTERNAL_FIELDS = new Set([
  '_id', '_mvcc_commit_ms', '_mvcc_version', '_raft_commit_index',
  '_raft_term', '_visibility_floor', '_visibility_state',
  '_logicalWritePayloadHash', 'created_at_ms', 'created_txn',
  'deleted_at_ms', 'deleted_txn', 'committed', 'tenant_id', 'version',
]);

function inferColumns(docs: Record<string, unknown>[]): string[] {
  const keys = new Set<string>();
  for (const doc of docs.slice(0, 10)) {
    for (const k of Object.keys(doc)) {
      if (!INTERNAL_FIELDS.has(k)) keys.add(k);
    }
  }
  // Pin _id first, then sort the rest
  const arr = [...keys].filter(k => k !== '_id');
  return ['_id', ...arr];
}

function CellValue({ value }: { value: unknown }) {
  if (value === null || value === undefined) return <span className="text-surface-600 italic">null</span>;
  if (typeof value === 'boolean') return <span className="text-purple-400 font-mono">{String(value)}</span>;
  if (typeof value === 'number') return <span className="text-warning font-mono">{String(value)}</span>;
  if (typeof value === 'string') {
    if (value.length > 80) return <span className="text-surface-200 font-mono selectable">{value.slice(0, 80)}…</span>;
    return <span className="text-success font-mono selectable">"{value}"</span>;
  }
  if (Array.isArray(value)) return <span className="text-surface-400 font-mono">[… {value.length} items]</span>;
  if (typeof value === 'object') return <span className="text-surface-400 font-mono">{'{ … }'}</span>;
  return <span className="text-surface-200 selectable">{String(value)}</span>;
}

export function DataBrowser() {
  const activeDatabase = useWorkspaceStore(s => s.activeDatabase);
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();

  const [docs, setDocs] = useState<Record<string, unknown>[]>([]);
  const [columns, setColumns] = useState<string[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [pageSize, setPageSize] = useState(50);
  const [loading, setLoading] = useState(false);
  const [viewMode, setViewMode] = useState<ViewMode>('table');
  const [filter, setFilter] = useState('{}');
  const [filterError, setFilterError] = useState('');
  const [showFilter, setShowFilter] = useState(false);
  const [selectedDoc, setSelectedDoc] = useState<Record<string, unknown> | null>(null);
  const [editingDoc, setEditingDoc] = useState<Record<string, unknown> | null>(null);
  const [deletingDoc, setDeletingDoc] = useState<Record<string, unknown> | null>(null);
  const [insertOpen, setInsertOpen] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [sortField, setSortField] = useState('');
  const [sortDir, setSortDir] = useState<'asc' | 'desc'>('asc');

  const offset = page * pageSize;

  const load = useCallback(async () => {
    if (!activeDatabase || !activeCollection || !activeProfileId) return;
    setLoading(true);
    setFilterError('');

    let parsedFilter: Record<string, unknown> = {};
    try {
      parsedFilter = JSON.parse(filter.trim() || '{}');
    } catch {
      setFilterError('Invalid JSON filter');
      setLoading(false);
      return;
    }

    try {
      const [docsRes, countRes] = await Promise.all([
        request<{ data?: Record<string, unknown>[] }>({
          action: 'find',
          collection: activeCollection,
          filter: parsedFilter,
          limit: pageSize,
          offset,
        }),
        request<{ count?: number }>({
          action: 'count',
          collection: activeCollection,
          filter: parsedFilter,
        }),
      ]);

      const fetched = docsRes.response.data ?? [];
      setDocs(fetched);
      setTotal(countRes.response.count ?? fetched.length);
      setColumns(inferColumns(fetched));
    } catch (e) {
      notify({ type: 'error', title: 'Query failed', message: String(e) });
      setDocs([]);
    } finally {
      setLoading(false);
    }
  }, [activeDatabase, activeCollection, activeProfileId, filter, pageSize, offset, request]);

  useEffect(() => {
    setPage(0);
    setDocs([]);
    setSelectedDoc(null);
  }, [activeDatabase, activeCollection]);

  useEffect(() => { load(); }, [activeDatabase, activeCollection, page, pageSize]);

  const handleDeleteDoc = async () => {
    if (!deletingDoc || !activeCollection) return;
    setDeleting(true);
    try {
      await request({
        action: 'deleteOne',
        collection: activeCollection,
        filter: { _id: deletingDoc._id },
      });
      notify({ type: 'success', title: 'Document deleted' });
      setDeletingDoc(null);
      setSelectedDoc(null);
      await load();
    } catch (e) {
      notify({ type: 'error', title: 'Delete failed', message: String(e) });
    } finally {
      setDeleting(false);
    }
  };

  const handleSort = (col: string) => {
    if (sortField === col) setSortDir(d => d === 'asc' ? 'desc' : 'asc');
    else { setSortField(col); setSortDir('asc'); }
  };

  const copyDoc = (doc: Record<string, unknown>) => {
    navigator.clipboard.writeText(JSON.stringify(doc, null, 2));
    notify({ type: 'success', title: 'Copied to clipboard', duration: 2000 });
  };

  const exportJson = () => {
    const blob = new Blob([JSON.stringify(docs, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `${activeCollection ?? 'data'}-export.json`;
    a.click();
    URL.revokeObjectURL(url);
  };

  if (!activeDatabase || !activeCollection) {
    return (
      <EmptyState
        icon={<AlignLeft size={20} />}
        title="No collection selected"
        description="Select a collection from the sidebar to browse documents."
        className="h-full"
      />
    );
  }

  const totalPages = Math.max(1, Math.ceil(total / pageSize));

  return (
    <div className="h-full flex flex-col">
      {/* Toolbar */}
      <div className="flex items-center gap-2 px-4 py-2 border-b border-surface-500/30 flex-shrink-0 bg-surface-800/50">
        <div className="flex items-center gap-1.5 min-w-0">
          <span className="text-xs font-mono text-pacific-300">{activeDatabase}</span>
          <span className="text-surface-600 text-xs">›</span>
          <span className="text-xs font-mono text-surface-100 font-semibold">{activeCollection}</span>
          <span className="text-2xs text-surface-500 ml-1">({total} docs)</span>
        </div>

        <div className="flex-1" />

        {/* View mode */}
        <div className="flex bg-surface-700 rounded p-0.5 gap-0.5">
          {(['table', 'json', 'raw'] as ViewMode[]).map(mode => (
            <button
              key={mode}
              onClick={() => setViewMode(mode)}
              className={clsx(
                'px-2 py-1 rounded text-2xs font-medium transition-colors',
                viewMode === mode ? 'bg-surface-500 text-surface-50' : 'text-surface-400 hover:text-surface-200',
              )}
              id={`view-${mode}`}
              title={`${mode} view`}
            >
              {mode === 'table' ? <LayoutGrid size={11} /> : mode === 'json' ? <Code size={11} /> : <AlignLeft size={11} />}
            </button>
          ))}
        </div>

        <Button variant="ghost" size="xs" icon={<Filter size={11} />} onClick={() => setShowFilter(f => !f)} id="toggle-filter-btn">
          Filter
        </Button>
        <Button variant="ghost" size="xs" icon={<RefreshCw size={11} />} onClick={load} loading={loading} id="refresh-data-btn">
          Refresh
        </Button>
        <Button variant="ghost" size="xs" icon={<Download size={11} />} onClick={exportJson} id="export-json-btn">
          Export
        </Button>
        <Button variant="primary" size="xs" icon={<Plus size={11} />} onClick={() => setInsertOpen(true)} id="insert-doc-btn">
          Insert
        </Button>
      </div>

      {/* Filter bar */}
      {showFilter && (
        <div className="px-4 py-2 border-b border-surface-500/30 bg-surface-800/30 flex items-center gap-2">
          <Search size={12} className="text-surface-400 flex-shrink-0" />
          <Input
            id="doc-filter-input"
            value={filter}
            onChange={e => setFilter(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') { setPage(0); load(); } }}
            placeholder='{"field": "value"}'
            className="font-mono flex-1 py-1"
            error={filterError || undefined}
          />
          <Button variant="primary" size="xs" onClick={() => { setPage(0); load(); }} id="apply-filter-btn">Apply</Button>
          <Button variant="ghost" size="xs" onClick={() => { setFilter('{}'); setPage(0); load(); }}>Clear</Button>
        </div>
      )}

      {/* Main area */}
      <div className="flex-1 overflow-hidden flex">
        {/* Document grid */}
        <div className="flex-1 overflow-auto">
          {loading && docs.length === 0 ? (
            <SkeletonBlock rows={8} />
          ) : docs.length === 0 ? (
            <EmptyState
              icon={<AlignLeft size={18} />}
              title="No documents"
              description="This collection is empty. Insert a document to get started."
              action={<Button variant="primary" size="sm" icon={<Plus size={12} />} onClick={() => setInsertOpen(true)}>Insert Document</Button>}
              compact
              className="py-16"
            />
          ) : viewMode === 'table' ? (
            <table className="w-full text-xs border-collapse">
              <thead className="sticky top-0 z-10 bg-surface-800 border-b border-surface-500/30">
                <tr>
                  <th className="w-8 px-2 py-2 text-left" />
                  {columns.map(col => (
                    <th
                      key={col}
                      onClick={() => handleSort(col)}
                      className="px-3 py-2 text-left font-medium text-surface-300 hover:text-surface-100 cursor-pointer whitespace-nowrap select-none border-r border-surface-500/20 last:border-0"
                    >
                      <div className="flex items-center gap-1">
                        {col}
                        {sortField === col && (
                          sortDir === 'asc' ? <SortAsc size={10} /> : <SortDesc size={10} />
                        )}
                      </div>
                    </th>
                  ))}
                  <th className="w-20 px-2 py-2" />
                </tr>
              </thead>
              <tbody>
                {docs.map((doc, i) => (
                  <tr
                    key={String(doc._id ?? i)}
                    onClick={() => setSelectedDoc(doc)}
                    className={clsx(
                      'border-b border-surface-500/20 hover:bg-surface-700/30 cursor-pointer transition-colors',
                      selectedDoc === doc && 'bg-pacific-500/10 border-pacific-500/20',
                    )}
                  >
                    <td className="px-2 py-1.5 text-surface-600 text-center">{offset + i + 1}</td>
                    {columns.map(col => (
                      <td key={col} className="px-3 py-1.5 max-w-xs border-r border-surface-500/10 last:border-0">
                        <div className="truncate"><CellValue value={doc[col]} /></div>
                      </td>
                    ))}
                    <td className="px-2 py-1.5">
                      <div className="flex items-center gap-1 opacity-0 group-hover:opacity-100">
                        <button
                          onClick={e => { e.stopPropagation(); setEditingDoc(doc); }}
                          className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-surface-200 transition-colors"
                          title="Edit"
                        >
                          <Edit2 size={10} />
                        </button>
                        <button
                          onClick={e => { e.stopPropagation(); copyDoc(doc); }}
                          className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-surface-200 transition-colors"
                          title="Copy JSON"
                        >
                          <Copy size={10} />
                        </button>
                        <button
                          onClick={e => { e.stopPropagation(); setDeletingDoc(doc); }}
                          className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-danger transition-colors"
                          title="Delete"
                        >
                          <Trash2 size={10} />
                        </button>
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : viewMode === 'json' ? (
            <div className="p-4 space-y-2">
              {docs.map((doc, i) => (
                <div key={String(doc._id ?? i)} className="bg-surface-800 border border-surface-500/30 rounded-lg p-3">
                  <pre className="text-xs text-surface-200 font-mono selectable whitespace-pre-wrap overflow-auto max-h-48">
                    {JSON.stringify(doc, null, 2)}
                  </pre>
                </div>
              ))}
            </div>
          ) : (
            <pre className="p-4 text-xs text-surface-300 font-mono selectable whitespace-pre overflow-auto h-full">
              {JSON.stringify(docs, null, 2)}
            </pre>
          )}
        </div>

        {/* Document detail panel */}
        {selectedDoc && (
          <div className="w-72 flex-shrink-0 border-l border-surface-500/30 bg-surface-800 flex flex-col">
            <div className="flex items-center justify-between px-3 py-2 border-b border-surface-500/30">
              <p className="text-xs font-semibold text-surface-200">Document</p>
              <div className="flex gap-1">
                <button onClick={() => setEditingDoc(selectedDoc)} className="p-1 rounded hover:bg-surface-600 text-surface-400 hover:text-surface-200" title="Edit">
                  <Edit2 size={11} />
                </button>
                <button onClick={() => copyDoc(selectedDoc)} className="p-1 rounded hover:bg-surface-600 text-surface-400 hover:text-surface-200" title="Copy">
                  <Copy size={11} />
                </button>
                <button onClick={() => setDeletingDoc(selectedDoc)} className="p-1 rounded hover:bg-surface-600 text-surface-400 hover:text-danger" title="Delete">
                  <Trash2 size={11} />
                </button>
                <button onClick={() => setSelectedDoc(null)} className="p-1 rounded hover:bg-surface-600 text-surface-400 hover:text-surface-200" title="Close">×</button>
              </div>
            </div>
            <div className="flex-1 overflow-auto p-3">
              <pre className="text-xs text-surface-200 font-mono selectable whitespace-pre-wrap">
                {JSON.stringify(selectedDoc, null, 2)}
              </pre>
            </div>
          </div>
        )}
      </div>

      {/* Pagination */}
      <div className="flex items-center justify-between px-4 py-2 border-t border-surface-500/30 flex-shrink-0 bg-surface-800/50">
        <div className="flex items-center gap-2 text-xs text-surface-400">
          <span>Rows:</span>
          <select
            value={pageSize}
            onChange={e => { setPageSize(Number(e.target.value)); setPage(0); }}
            className="bg-surface-700 border border-surface-500/30 rounded px-1.5 py-0.5 text-surface-200"
            id="page-size-select"
          >
            {PAGE_SIZE_OPTIONS.map(n => <option key={n} value={n}>{n}</option>)}
          </select>
          <span>
            {docs.length > 0 ? `${offset + 1}–${offset + docs.length} of ${total}` : '0'}
          </span>
        </div>
        <div className="flex items-center gap-1">
          <Button variant="ghost" size="xs" onClick={() => setPage(0)} disabled={page === 0}>«</Button>
          <Button variant="ghost" size="xs" icon={<ChevronLeft size={11} />} onClick={() => setPage(p => Math.max(0, p - 1))} disabled={page === 0} id="prev-page-btn" />
          <span className="text-xs text-surface-400 px-2">
            {page + 1} / {totalPages}
          </span>
          <Button variant="ghost" size="xs" icon={<ChevronRight size={11} />} onClick={() => setPage(p => Math.min(totalPages - 1, p + 1))} disabled={page >= totalPages - 1} id="next-page-btn" />
          <Button variant="ghost" size="xs" onClick={() => setPage(totalPages - 1)} disabled={page >= totalPages - 1}>»</Button>
        </div>
      </div>

      {/* Dialogs */}
      <InsertDocumentDialog
        open={insertOpen}
        collection={activeCollection}
        onClose={() => setInsertOpen(false)}
        onInserted={load}
      />

      {editingDoc && (
        <DocumentViewer
          doc={editingDoc}
          collection={activeCollection}
          mode="edit"
          onClose={() => setEditingDoc(null)}
          onSaved={() => { setEditingDoc(null); load(); }}
        />
      )}

      <ConfirmDialog
        open={!!deletingDoc}
        onClose={() => setDeletingDoc(null)}
        onConfirm={handleDeleteDoc}
        title="Delete Document"
        message={<>Are you sure you want to delete this document? <span className="code">{String(deletingDoc?._id ?? '')}</span> This action cannot be undone.</>}
        confirmLabel="Delete Document"
        loading={deleting}
        id="delete-doc-confirm"
      />
    </div>
  );
}
