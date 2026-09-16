import { useState } from 'react';
import { Dialog } from '../../components/ui/Dialog';
import { Button } from '../../components/ui/Button';
import { usePacific } from '../../hooks/usePacific';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { JsonEditor } from '../../components/ui/JsonEditor';

interface Props {
  open: boolean;
  collection: string;
  onClose: () => void;
  onInserted: () => void;
}

const PLACEHOLDER = `{
  "name": "example",
  "value": 42
}`;

export function InsertDocumentDialog({ open, collection, onClose, onInserted }: Props) {
  const [json, setJson] = useState(PLACEHOLDER);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const [bulk, setBulk] = useState(false);
  const { request } = usePacific();
  const notify = useWorkspaceStore(s => s.notify);

  const handleInsert = async () => {
    setError('');
    let parsed: unknown;
    try { parsed = JSON.parse(json); }
    catch (e) { setError('Invalid JSON: ' + (e instanceof Error ? e.message : String(e))); return; }

    setLoading(true);
    try {
      if (Array.isArray(parsed)) {
        await request({ action: 'insertMany', collection, data: parsed });
        notify({ type: 'success', title: `Inserted ${parsed.length} documents` });
      } else {
        await request({ action: 'insert', collection, data: parsed });
        notify({ type: 'success', title: 'Document inserted' });
      }
      onInserted();
      onClose();
      setJson(PLACEHOLDER);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  };

  return (
    <Dialog
      open={open}
      onClose={onClose}
      title={`Insert into ${collection}`}
      description="Enter a JSON document or array of documents to insert"
      width="lg"
      id="insert-doc-dialog"
      footer={
        <>
          <label className="flex items-center gap-1.5 text-xs text-surface-400 mr-auto cursor-pointer">
            <input
              type="checkbox"
              checked={bulk}
              onChange={e => {
                setBulk(e.target.checked);
                setJson(e.target.checked ? `[\n  ${PLACEHOLDER.trim()}\n]` : PLACEHOLDER);
              }}
              className="accent-pacific-500"
            />
            Bulk insert (array)
          </label>
          <Button variant="outline" size="sm" onClick={onClose}>Cancel</Button>
          <Button variant="primary" size="sm" onClick={handleInsert} loading={loading} id="confirm-insert-btn">
            Insert
          </Button>
        </>
      }
    >
      <div className="h-64">
        <JsonEditor
          value={json}
          onChange={setJson}
          placeholder="Enter JSON document…"
        />
      </div>
      {error && <p className="text-xs text-danger mt-2">{error}</p>}
    </Dialog>
  );
}
