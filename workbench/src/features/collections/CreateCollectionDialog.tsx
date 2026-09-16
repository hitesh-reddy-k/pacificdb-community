import { useState } from 'react';
import { Dialog } from '../../components/ui/Dialog';
import { Input } from '../../components/ui/Input';
import { Button } from '../../components/ui/Button';
import { usePacific } from '../../hooks/usePacific';
import { useWorkspaceStore } from '../../stores/workspace-store';

interface Props {
  open: boolean;
  database: string;
  onClose: () => void;
  onCreated: () => void;
}

export function CreateCollectionDialog({ open, database, onClose, onCreated }: Props) {
  const [name, setName] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const { request } = usePacific();
  const notify = useWorkspaceStore(s => s.notify);
  const addCollection = useWorkspaceStore(s => s.addCollection);
  const setActiveDatabase = useWorkspaceStore(s => s.setActiveDatabase);

  const handleCreate = async () => {
    const trimmed = name.trim();
    if (!trimmed) { setError('Collection name is required'); return; }

    setLoading(true);
    setError('');
    try {
      // Must set the active database before calling createCollection
      setActiveDatabase(database);
      await request({ action: 'createCollection', collection: trimmed });
      addCollection(database, trimmed);
      notify({ type: 'success', title: `Collection "${trimmed}" created` });
      setName('');
      onCreated();
      onClose();
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
      title="Create Collection"
      description={database ? `Create a collection in database "${database}"` : 'Create a new collection'}
      width="sm"
      id="create-collection-dialog"
      footer={
        <>
          <Button variant="outline" size="sm" onClick={onClose} disabled={loading}>Cancel</Button>
          <Button variant="primary" size="sm" onClick={handleCreate} loading={loading} id="create-coll-btn">
            Create
          </Button>
        </>
      }
    >
      <Input
        label="Collection Name"
        id="new-coll-name"
        value={name}
        onChange={e => { setName(e.target.value); setError(''); }}
        onKeyDown={e => { if (e.key === 'Enter') handleCreate(); }}
        placeholder="users"
        error={error}
        autoFocus
        className="font-mono"
      />
    </Dialog>
  );
}
