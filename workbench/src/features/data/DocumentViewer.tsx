import { useState } from 'react';
import { Dialog } from '../../components/ui/Dialog';
import { Button } from '../../components/ui/Button';
import { usePacific } from '../../hooks/usePacific';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { JsonEditor } from '../../components/ui/JsonEditor';

interface Props {
  doc: Record<string, unknown>;
  collection: string;
  mode: 'edit' | 'view';
  onClose: () => void;
  onSaved?: () => void;
}

export function DocumentViewer({ doc, collection, mode, onClose, onSaved }: Props) {
  const [json, setJson] = useState(JSON.stringify(doc, null, 2));
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const { request } = usePacific();
  const notify = useWorkspaceStore(s => s.notify);

  const handleSave = async () => {
    setError('');
    let parsed: Record<string, unknown>;
    try { parsed = JSON.parse(json); }
    catch (e) { setError('Invalid JSON: ' + (e instanceof Error ? e.message : String(e))); return; }

    if (!doc._id) { setError('Document has no _id field'); return; }

    setLoading(true);
    try {
      // Remove _id from update payload — it's used in the filter
      const { _id, ...update } = parsed;
      void _id; // suppress unused warning
      await request({
        action: 'updateOne',
        collection,
        filter: { _id: doc._id },
        update,
      });
      notify({ type: 'success', title: 'Document updated' });
      onSaved?.();
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  };

  return (
    <Dialog
      open
      onClose={onClose}
      title={mode === 'edit' ? 'Edit Document' : 'View Document'}
      description={`_id: ${String(doc._id ?? 'unknown')}`}
      width="lg"
      id="document-viewer-dialog"
      footer={mode === 'edit' ? (
        <>
          <Button variant="outline" size="sm" onClick={onClose}>Cancel</Button>
          <Button variant="primary" size="sm" onClick={handleSave} loading={loading} id="save-doc-btn">
            Save Changes
          </Button>
        </>
      ) : (
        <Button variant="ghost" size="sm" onClick={onClose}>Close</Button>
      )}
    >
      <div className="h-72">
        <JsonEditor
          value={json}
          onChange={setJson}
          readOnly={mode === 'view'}
        />
      </div>
      {error && <p className="text-xs text-danger mt-2">{error}</p>}
    </Dialog>
  );
}
