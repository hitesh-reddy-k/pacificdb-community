import { useState, useEffect, useCallback } from 'react';
import { ArchiveRestore, Plus, Trash2, CheckCircle2, RefreshCw, Download, RotateCcw, AlertTriangle } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { ConfirmDialog } from '../../components/ui/Dialog';
import { EmptyState, SkeletonBlock } from '../../components/ui/EmptyState';

interface Backup {
  id: string;
  description?: string;
  created_at?: string;
  status?: string;
  size_bytes?: number;
}

function fmtSize(bytes?: number) {
  if (!bytes) return '—';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

export function BackupManager() {
  const notify = useWorkspaceStore(s => s.notify);
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const { request } = usePacific();

  const [backups, setBackups] = useState<Backup[]>([]);
  const [loading, setLoading] = useState(false);
  const [creating, setCreating] = useState(false);
  const [verifying, setVerifying] = useState<string | null>(null);
  const [deleteTarget, setDeleteTarget] = useState<Backup | null>(null);
  const [restoreTarget, setRestoreTarget] = useState<Backup | null>(null);
  const [deleting, setDeleting] = useState(false);
  const [restoring, setRestoring] = useState(false);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const { response } = await request<{ backups?: Backup[] }>({ action: 'list_backups' });
      setBackups(response.backups ?? []);
    } catch {
      setBackups([]);
    } finally {
      setLoading(false);
    }
  }, [request]);

  useEffect(() => { load(); }, [activeProfileId]);

  const createBackup = async () => {
    setCreating(true);
    try {
      await request({ action: 'create_backup', description: 'manual backup' });
      notify({ type: 'success', title: 'Backup created' });
      await load();
    } catch (e) {
      notify({ type: 'error', title: 'Backup failed', message: String(e) });
    } finally {
      setCreating(false);
    }
  };

  const verify = async (b: Backup) => {
    setVerifying(b.id);
    try {
      const { response } = await request<{ valid?: boolean }>({ action: 'verify_backup', backup_id: b.id });
      const valid = (response as { valid?: boolean }).valid !== false;
      notify({ type: valid ? 'success' : 'warning', title: valid ? `Backup ${b.id.slice(0, 8)}… is valid` : 'Backup verification failed' });
    } catch (e) {
      notify({ type: 'error', title: 'Verify failed', message: String(e) });
    } finally {
      setVerifying(null);
    }
  };

  const handleDelete = async () => {
    if (!deleteTarget) return;
    setDeleting(true);
    try {
      await request({ action: 'delete_backup', backup_id: deleteTarget.id });
      notify({ type: 'success', title: 'Backup deleted' });
      setDeleteTarget(null);
      await load();
    } catch (e) {
      notify({ type: 'error', title: 'Delete failed', message: String(e) });
    } finally {
      setDeleting(false);
    }
  };

  const handleRestore = async () => {
    if (!restoreTarget) return;
    setRestoring(true);
    try {
      await request({ action: 'restore_backup', backup_id: restoreTarget.id });
      notify({ type: 'success', title: 'Restore initiated', message: 'Server is restoring the backup. This may take a moment.' });
      setRestoreTarget(null);
    } catch (e) {
      notify({ type: 'error', title: 'Restore failed', message: String(e) });
    } finally {
      setRestoring(false);
    }
  };

  const exportBackup = async (b: Backup) => {
    if (!activeProfileId) return;
    const result = await window.pacific.dialog.saveFile({
      title: 'Export Backup',
      defaultPath: `backup-${b.id.slice(0, 8)}.json`,
      filters: [{ name: 'JSON', extensions: ['json'] }],
    }) as { canceled?: boolean; filePath?: string };
    if (result.canceled || !result.filePath) return;
    try {
      await window.pacific.backup.exportToFile(activeProfileId, b.id, result.filePath);
      notify({ type: 'success', title: 'Backup exported', message: result.filePath });
    } catch (e) {
      notify({ type: 'error', title: 'Export failed', message: String(e) });
    }
  };

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <ArchiveRestore size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Backup Manager</span>
        <div className="flex-1" />
        <Button variant="ghost" size="xs" icon={<RefreshCw size={11} />} onClick={load} loading={loading}>Refresh</Button>
        <Button variant="primary" size="xs" icon={<Plus size={11} />} onClick={createBackup} loading={creating} id="create-backup-btn">
          Create Backup
        </Button>
      </div>

      <div className="flex-1 overflow-auto p-4">
        {loading ? <SkeletonBlock rows={4} /> : backups.length === 0 ? (
          <EmptyState
            icon={<ArchiveRestore size={20} />}
            title="No backups"
            description="Create a backup to protect your data."
            action={<Button variant="primary" size="sm" icon={<Plus size={12} />} onClick={createBackup} loading={creating}>Create Backup</Button>}
          />
        ) : (
          <div className="space-y-2">
            {backups.map(b => (
              <div key={b.id} className="card flex items-center gap-4">
                <div className="p-2 rounded-lg bg-surface-700">
                  <ArchiveRestore size={14} className="text-pacific-400" />
                </div>
                <div className="flex-1 min-w-0">
                  <p className="font-mono text-sm text-surface-100 truncate">{b.id}</p>
                  <p className="text-xs text-surface-400 mt-0.5">
                    {b.description ?? 'manual backup'}
                    {b.created_at ? ` · ${new Date(b.created_at).toLocaleString()}` : ''}
                    {b.size_bytes ? ` · ${fmtSize(b.size_bytes)}` : ''}
                    {b.status ? ` · ${b.status}` : ''}
                  </p>
                </div>
                <div className="flex gap-1.5">
                  <Button variant="ghost" size="xs" icon={<CheckCircle2 size={11} />} loading={verifying === b.id} onClick={() => verify(b)} id={`verify-${b.id}`}>Verify</Button>
                  <Button variant="ghost" size="xs" icon={<Download size={11} />} onClick={() => exportBackup(b)} id={`export-${b.id}`}>Export</Button>
                  <Button variant="ghost" size="xs" icon={<RotateCcw size={11} />} onClick={() => setRestoreTarget(b)} id={`restore-${b.id}`}>Restore</Button>
                  <Button variant="danger" size="xs" icon={<Trash2 size={11} />} onClick={() => setDeleteTarget(b)} id={`delete-backup-${b.id}`}>Delete</Button>
                </div>
              </div>
            ))}
          </div>
        )}
      </div>

      <ConfirmDialog
        open={!!deleteTarget}
        onClose={() => setDeleteTarget(null)}
        onConfirm={handleDelete}
        title="Delete Backup"
        message={<>Permanently delete backup <span className="code">{deleteTarget?.id.slice(0, 16)}…</span>? This cannot be undone.</>}
        confirmLabel="Delete Backup"
        loading={deleting}
        id="delete-backup-confirm"
      />

      <ConfirmDialog
        open={!!restoreTarget}
        onClose={() => setRestoreTarget(null)}
        onConfirm={handleRestore}
        title="Restore Backup"
        variant="warning"
        message={
          <div className="space-y-2">
            <div className="flex items-start gap-2 text-warning"><AlertTriangle size={14} className="flex-shrink-0 mt-0.5" /><p>Restoring will overwrite current data. This action cannot be undone.</p></div>
            <p>Restore backup: <span className="code">{restoreTarget?.id.slice(0, 16)}…</span></p>
          </div>
        }
        confirmLabel="Restore Backup"
        loading={restoring}
        id="restore-backup-confirm"
      />
    </div>
  );
}
