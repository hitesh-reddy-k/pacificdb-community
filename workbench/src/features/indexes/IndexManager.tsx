import { useState, useEffect, useCallback } from 'react';
import {
  Plus, Trash2, RefreshCw, Cpu, AlertCircle, CheckCircle2, Wrench,
} from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Input, Select } from '../../components/ui/Input';
import { Dialog, ConfirmDialog } from '../../components/ui/Dialog';
import { EmptyState, SkeletonBlock } from '../../components/ui/EmptyState';
import { useWorkspaceStore as ws } from '../../stores/workspace-store';

interface IndexDef {
  name?: string;
  field?: string;
  type?: string;
  unique?: boolean;
  [k: string]: unknown;
}

interface Index {
  name: string;
  definition: IndexDef;
  status?: string;
}

export function IndexManager() {
  const activeDatabase = useWorkspaceStore(s => s.activeDatabase);
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();

  const [indexes, setIndexes] = useState<Index[]>([]);
  const [loading, setLoading] = useState(false);
  const [createOpen, setCreateOpen] = useState(false);
  const [dropTarget, setDropTarget] = useState<string | null>(null);
  const [rebuilding, setRebuilding] = useState<string | null>(null);
  const [dropping, setDropping] = useState(false);

  // Create form
  const [indexName, setIndexName] = useState('');
  const [indexField, setIndexField] = useState('');
  const [indexType, setIndexType] = useState('hash');
  const [indexUnique, setIndexUnique] = useState(false);
  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState('');

  const load = useCallback(async () => {
    if (!activeCollection) return;
    setLoading(true);
    try {
      const { response } = await request<{ indexes?: Index[] }>({
        action: 'listSecondaryIndexes',
        collection: activeCollection,
      });
      setIndexes(response.indexes ?? []);
    } catch {
      setIndexes([]);
    } finally {
      setLoading(false);
    }
  }, [activeCollection, request]);

  useEffect(() => { load(); }, [activeCollection]);

  const handleCreate = async () => {
    if (!indexName.trim() || !indexField.trim()) {
      setCreateError('Name and field are required');
      return;
    }
    if (!activeCollection) return;
    setCreating(true);
    setCreateError('');
    try {
      await request({
        action: 'createSecondaryIndex',
        collection: activeCollection,
        definition: { name: indexName.trim(), field: indexField.trim(), type: indexType, unique: indexUnique },
      });
      notify({ type: 'success', title: `Index "${indexName}" created` });
      setCreateOpen(false);
      setIndexName(''); setIndexField('');
      await load();
    } catch (e) {
      setCreateError(String(e));
    } finally {
      setCreating(false);
    }
  };

  const handleDrop = async () => {
    if (!dropTarget || !activeCollection) return;
    setDropping(true);
    try {
      await request({ action: 'dropSecondaryIndex', collection: activeCollection, name: dropTarget });
      notify({ type: 'success', title: `Index "${dropTarget}" dropped` });
      setDropTarget(null);
      await load();
    } catch (e) {
      notify({ type: 'error', title: 'Drop failed', message: String(e) });
    } finally {
      setDropping(false);
    }
  };

  const handleRebuild = async (name: string) => {
    if (!activeCollection) return;
    setRebuilding(name);
    try {
      await request({ action: 'rebuildSecondaryIndex', collection: activeCollection, name });
      notify({ type: 'success', title: `Index "${name}" rebuild started` });
    } catch (e) {
      notify({ type: 'error', title: 'Rebuild failed', message: String(e) });
    } finally {
      setRebuilding(null);
    }
  };

  if (!activeCollection) {
    return (
      <EmptyState icon={<Cpu size={20} />} title="No collection selected" description="Select a collection to manage its indexes." className="h-full" />
    );
  }

  return (
    <div className="h-full flex flex-col">
      {/* Header */}
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <Cpu size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Secondary Indexes</span>
        <span className="text-2xs text-surface-500 font-mono ml-1">{activeCollection}</span>
        <div className="flex-1" />
        <Button variant="ghost" size="xs" icon={<RefreshCw size={11} />} onClick={load} loading={loading}>Refresh</Button>
        <Button variant="primary" size="xs" icon={<Plus size={11} />} onClick={() => setCreateOpen(true)} id="create-index-btn">
          Create Index
        </Button>
      </div>

      {/* Index list */}
      <div className="flex-1 overflow-auto p-4">
        {loading ? <SkeletonBlock rows={4} /> :
          indexes.length === 0 ? (
            <EmptyState
              icon={<Cpu size={18} />}
              title="No secondary indexes"
              description="Create a secondary index to speed up queries on specific fields."
              action={<Button variant="primary" size="sm" icon={<Plus size={12} />} onClick={() => setCreateOpen(true)}>Create Index</Button>}
              compact
            />
          ) : (
            <div className="space-y-2">
              {indexes.map(idx => (
                <div key={idx.name} className="card flex items-center gap-4">
                  <div className="p-2 rounded-lg bg-surface-700">
                    <Cpu size={14} className="text-pacific-400" />
                  </div>
                  <div className="flex-1 min-w-0">
                    <div className="flex items-center gap-2">
                      <p className="font-mono text-sm font-semibold text-surface-100">{idx.name}</p>
                      {idx.definition.unique && <span className="badge-purple">unique</span>}
                      {idx.status && (
                        <span className={idx.status === 'ready' ? 'badge-green' : 'badge-yellow'}>{idx.status}</span>
                      )}
                    </div>
                    <p className="text-xs text-surface-400 mt-0.5 font-mono">
                      field: {String(idx.definition.field ?? '—')} · type: {String(idx.definition.type ?? 'hash')}
                    </p>
                  </div>
                  <div className="flex gap-2">
                    <Button
                      variant="ghost" size="xs"
                      icon={<Wrench size={11} />}
                      loading={rebuilding === idx.name}
                      onClick={() => handleRebuild(idx.name)}
                      id={`rebuild-${idx.name}`}
                    >
                      Rebuild
                    </Button>
                    <Button
                      variant="danger" size="xs"
                      icon={<Trash2 size={11} />}
                      onClick={() => setDropTarget(idx.name)}
                      id={`drop-${idx.name}`}
                    >
                      Drop
                    </Button>
                  </div>
                </div>
              ))}
            </div>
          )}
      </div>

      {/* Create dialog */}
      <Dialog
        open={createOpen}
        onClose={() => setCreateOpen(false)}
        title="Create Secondary Index"
        description={`Create a new index on collection "${activeCollection}"`}
        width="sm"
        id="create-index-dialog"
        footer={
          <>
            <Button variant="outline" size="sm" onClick={() => setCreateOpen(false)}>Cancel</Button>
            <Button variant="primary" size="sm" onClick={handleCreate} loading={creating} id="confirm-create-index-btn">Create</Button>
          </>
        }
      >
        <div className="flex flex-col gap-3">
          <Input label="Index Name" id="idx-name" value={indexName} onChange={e => setIndexName(e.target.value)} placeholder="idx_field_name" className="font-mono" />
          <Input label="Field" id="idx-field" value={indexField} onChange={e => setIndexField(e.target.value)} placeholder="fieldName" className="font-mono" />
          <Select label="Type" id="idx-type" value={indexType} onChange={e => setIndexType(e.target.value)}>
            <option value="hash">Hash</option>
            <option value="btree">B-Tree</option>
            <option value="range">Range</option>
          </Select>
          <label className="flex items-center gap-2 cursor-pointer">
            <input type="checkbox" checked={indexUnique} onChange={e => setIndexUnique(e.target.checked)} className="accent-pacific-500" />
            <span className="text-sm text-surface-200">Unique constraint</span>
          </label>
          {createError && <p className="text-xs text-danger">{createError}</p>}
        </div>
      </Dialog>

      <ConfirmDialog
        open={!!dropTarget}
        onClose={() => setDropTarget(null)}
        onConfirm={handleDrop}
        title="Drop Index"
        message={<>Drop index <span className="code">{dropTarget}</span>? This cannot be undone.</>}
        confirmLabel="Drop Index"
        loading={dropping}
        id="drop-index-confirm"
      />
    </div>
  );
}
