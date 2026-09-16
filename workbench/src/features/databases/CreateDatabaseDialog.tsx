import { useState } from 'react';
import { Dialog } from '../../components/ui/Dialog';
import { Input } from '../../components/ui/Input';
import { Button } from '../../components/ui/Button';
import { usePacific } from '../../hooks/usePacific';
import { useWorkspaceStore } from '../../stores/workspace-store';

interface Props {
  open: boolean;
  onClose: () => void;
  onCreated: () => void;
}

export function CreateDatabaseDialog({ open, onClose, onCreated }: Props) {
  const [name, setName] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const { request } = usePacific();
  const notify = useWorkspaceStore(s => s.notify);
  const addDatabase = useWorkspaceStore(s => s.addDatabase);

  const handleCreate = async () => {
    const trimmed = name.trim();
    if (!trimmed) { setError('Database name is required'); return; }
    if (!/^[a-zA-Z0-9_-]+$/.test(trimmed)) { setError('Use only letters, numbers, _ and -'); return; }

    setLoading(true);
    setError('');
    try {
      await request({ action: 'createDatabase', dbName: trimmed, dbType: 'binary' });
      addDatabase(trimmed);
      notify({ type: 'success', title: `Database "${trimmed}" created` });
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
      title="Create Database"
      description="Create a new database on the connected PacificDB server"
      width="sm"
      id="create-database-dialog"
      footer={
        <>
          <Button variant="outline" size="sm" onClick={onClose} disabled={loading}>Cancel</Button>
          <Button variant="primary" size="sm" onClick={handleCreate} loading={loading} id="create-db-btn">
            Create
          </Button>
        </>
      }
    >
      <Input
        label="Database Name"
        id="new-db-name"
        value={name}
        onChange={e => { setName(e.target.value); setError(''); }}
        onKeyDown={e => { if (e.key === 'Enter') handleCreate(); }}
        placeholder="mydb"
        error={error}
        autoFocus
        className="font-mono"
        hint="Letters, numbers, underscores, and hyphens only"
      />
    </Dialog>
  );
}
