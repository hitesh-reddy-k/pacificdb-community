import { useState, useEffect, useCallback } from 'react';
import { ArchiveRestore, Upload, Download, Trash2, RefreshCw, Image, File, Play } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { ConfirmDialog } from '../../components/ui/Dialog';
import { EmptyState, SkeletonBlock } from '../../components/ui/EmptyState';
import { clsx } from 'clsx';

interface MediaItem {
  id: string;
  filename?: string;
  content_type?: string;
  size_bytes?: number;
  status?: string;
  sha256?: string;
  created_at?: string;
}

function fmtSize(bytes?: number) {
  if (!bytes) return '—';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function MediaIcon({ contentType }: { contentType?: string }) {
  if (contentType?.startsWith('image/')) return <Image size={16} className="text-purple-400" />;
  if (contentType?.startsWith('video/')) return <Play size={16} className="text-pacific-400" />;
  return <File size={16} className="text-surface-400" />;
}

export function MediaManager() {
  const notify = useWorkspaceStore(s => s.notify);
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const { request } = usePacific();

  const [items, setItems] = useState<MediaItem[]>([]);
  const [loading, setLoading] = useState(false);
  const [showAll, setShowAll] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [uploadProgress, setUploadProgress] = useState<{ index: number; total: number } | null>(null);
  const [deleteTarget, setDeleteTarget] = useState<MediaItem | null>(null);
  const [deleting, setDeleting] = useState(false);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const { response } = await request<{ media?: MediaItem[] }>({
        action: 'community_media_list',
        all: showAll,
      });
      setItems(response.media ?? []);
    } catch { setItems([]); }
    finally { setLoading(false); }
  }, [request, showAll]);

  useEffect(() => { load(); }, [activeProfileId, showAll]);

  // Subscribe to upload progress from main process
  useEffect(() => {
    const unsub = window.pacific.media.onProgress((event) => {
      const ev = event as { index: number; total: number };
      setUploadProgress(ev);
    });
    return () => { if (typeof unsub === 'function') unsub(); };
  }, []);

  const handleUpload = async () => {
    if (!activeProfileId) return;
    const result = await window.pacific.dialog.openFile({
      title: 'Select media file',
      properties: ['openFile'],
      filters: [
        { name: 'Images', extensions: ['jpg', 'jpeg', 'png', 'gif', 'webp'] },
        { name: 'Videos', extensions: ['mp4', 'mov', 'avi', 'mkv', 'webm'] },
        { name: 'All Files', extensions: ['*'] },
      ],
    }) as { canceled?: boolean; filePaths?: string[] };

    if (result.canceled || !result.filePaths?.[0]) return;

    const filePath = result.filePaths[0];
    const ext = filePath.split('.').pop()?.toLowerCase() ?? '';
    const contentTypeMap: Record<string, string> = {
      jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png',
      gif: 'image/gif', webp: 'image/webp',
      mp4: 'video/mp4', mov: 'video/quicktime', avi: 'video/avi',
      mkv: 'video/x-matroska', webm: 'video/webm',
    };
    const contentType = contentTypeMap[ext] ?? 'application/octet-stream';

    setUploading(true);
    setUploadProgress(null);
    try {
      await window.pacific.media.uploadFile(activeProfileId, {
        filePath,
        collection: 'media',
        contentType,
      });
      notify({ type: 'success', title: 'Media uploaded' });
      await load();
    } catch (e) {
      notify({ type: 'error', title: 'Upload failed', message: String(e) });
    } finally {
      setUploading(false);
      setUploadProgress(null);
    }
  };

  const handleDownload = async (item: MediaItem) => {
    if (!activeProfileId) return;
    const result = await window.pacific.dialog.saveFile({
      title: 'Save media file',
      defaultPath: item.filename ?? item.id,
    }) as { canceled?: boolean; filePath?: string };
    if (result.canceled || !result.filePath) return;
    try {
      await window.pacific.media.downloadFile(activeProfileId, item.id, result.filePath);
      notify({ type: 'success', title: 'Media downloaded', message: result.filePath });
    } catch (e) {
      notify({ type: 'error', title: 'Download failed', message: String(e) });
    }
  };

  const handleDelete = async () => {
    if (!deleteTarget) return;
    setDeleting(true);
    try {
      await request({ action: 'community_media_delete', media_id: deleteTarget.id });
      notify({ type: 'success', title: 'Media deleted' });
      setDeleteTarget(null);
      await load();
    } catch (e) {
      notify({ type: 'error', title: 'Delete failed', message: String(e) });
    } finally {
      setDeleting(false);
    }
  };

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <ArchiveRestore size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Media</span>
        <div className="flex-1" />
        <label className="flex items-center gap-1.5 text-xs text-surface-400 cursor-pointer mr-2">
          <input type="checkbox" checked={showAll} onChange={e => setShowAll(e.target.checked)} className="accent-pacific-500" />
          Show incomplete
        </label>
        <Button variant="ghost" size="xs" icon={<RefreshCw size={11} />} onClick={load} loading={loading}>Refresh</Button>
        <Button
          variant="primary" size="xs"
          icon={uploading ? <RefreshCw size={11} className="animate-spin" /> : <Upload size={11} />}
          onClick={handleUpload}
          loading={uploading}
          id="upload-media-btn"
        >
          {uploading ? (uploadProgress ? `${uploadProgress.index + 1}/${uploadProgress.total}` : 'Uploading…') : 'Upload'}
        </Button>
      </div>

      <div className="flex-1 overflow-auto p-4">
        {loading ? <SkeletonBlock rows={4} /> : items.length === 0 ? (
          <EmptyState
            icon={<ArchiveRestore size={20} />}
            title="No media files"
            description="Upload images, videos, or other files."
            action={<Button variant="primary" size="sm" icon={<Upload size={12} />} onClick={handleUpload} loading={uploading}>Upload Media</Button>}
          />
        ) : (
          <div className="grid grid-cols-1 gap-2">
            {items.map(item => (
              <div key={item.id} className="card flex items-center gap-4">
                <div className="p-2 rounded-lg bg-surface-700 flex-shrink-0">
                  <MediaIcon contentType={item.content_type} />
                </div>
                <div className="flex-1 min-w-0">
                  <p className="text-sm font-medium text-surface-100 truncate">{item.filename ?? item.id}</p>
                  <p className="text-xs text-surface-400 mt-0.5 font-mono">
                    {item.content_type ?? 'unknown'}
                    {item.size_bytes ? ` · ${fmtSize(item.size_bytes)}` : ''}
                    {item.status ? ` · ${item.status}` : ''}
                  </p>
                  {item.sha256 && (
                    <p className="text-2xs text-surface-600 mt-0.5 truncate font-mono">{item.sha256}</p>
                  )}
                </div>
                <div className="flex gap-1.5">
                  <Button variant="ghost" size="xs" icon={<Download size={11} />} onClick={() => handleDownload(item)} id={`download-${item.id}`}>
                    Download
                  </Button>
                  <Button variant="danger" size="xs" icon={<Trash2 size={11} />} onClick={() => setDeleteTarget(item)} id={`delete-media-${item.id}`}>
                    Delete
                  </Button>
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
        title="Delete Media"
        message={<>Delete <span className="code">{deleteTarget?.filename ?? deleteTarget?.id}</span>? This will also delete all chunks and cannot be undone.</>}
        confirmLabel="Delete Media"
        loading={deleting}
        id="delete-media-confirm"
      />
    </div>
  );
}
