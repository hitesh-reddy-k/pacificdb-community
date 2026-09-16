import {
  Database, Search, Activity, Shield, Server, ArchiveRestore,
  PanelLeft, Settings, History, Bookmark, Brain, Terminal,
  ChevronDown, ChevronRight, Plus, RefreshCw, Layers, Cpu, Network,
} from 'lucide-react';
import { clsx } from 'clsx';
import { useConnectionStore } from '../stores/connection-store';
import { useWorkspaceStore, type ActiveView } from '../stores/workspace-store';
import { usePacific } from '../hooks/usePacific';
import { Button } from '../components/ui/Button';
import { useState, useCallback } from 'react';
import { CreateDatabaseDialog } from '../features/databases/CreateDatabaseDialog';
import { CreateCollectionDialog } from '../features/collections/CreateCollectionDialog';

const NAV_SECTIONS = [
  {
    label: 'Database',
    items: [
      { id: 'overview', label: 'Overview', icon: Activity },
      { id: 'data', label: 'Data Browser', icon: Database },
      { id: 'query', label: 'Query', icon: Search },
      { id: 'aggregate', label: 'Aggregation', icon: Layers },
      { id: 'indexes', label: 'Indexes', icon: Cpu },
    ],
  },
  {
    label: 'AI & Search',
    items: [
      { id: 'vectors', label: 'Vector Search', icon: Brain },
      { id: 'nlq', label: 'Natural Language', icon: Brain },
    ],
  },
  {
    label: 'Storage',
    items: [
      { id: 'media', label: 'Media', icon: ArchiveRestore },
      { id: 'backups', label: 'Backups', icon: ArchiveRestore },
    ],
  },
  {
    label: 'Administration',
    items: [
      { id: 'security', label: 'Security', icon: Shield },
      { id: 'cluster', label: 'Cluster', icon: Network },
      { id: 'metrics', label: 'Metrics', icon: Activity },
    ],
  },
  {
    label: 'Developer',
    items: [
      { id: 'console', label: 'Raw Console', icon: Terminal },
      { id: 'history', label: 'History', icon: History },
      { id: 'saved', label: 'Saved Queries', icon: Bookmark },
    ],
  },
] as const;

export function Sidebar() {
  const profiles = useConnectionStore(s => s.profiles);
  const statuses = useConnectionStore(s => s.statuses);
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const activeView = useWorkspaceStore(s => s.activeView);
  const activeDatabase = useWorkspaceStore(s => s.activeDatabase);
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const databases = useWorkspaceStore(s => s.databases);
  const expandedDbs = useWorkspaceStore(s => s.expandedDbs);
  const dbListLoading = useWorkspaceStore(s => s.dbListLoading);
  const { navigateTo, toggleDbExpanded, setActiveDatabase, setActiveCollection, setDbListLoading, setDatabases, notify } = useWorkspaceStore();
  const setSidebarCollapsed = useWorkspaceStore(s => s.setSidebarCollapsed);
  const { request } = usePacific();

  const [createDbOpen, setCreateDbOpen] = useState(false);
  const [createCollOpen, setCreateCollOpen] = useState(false);
  const [targetDb, setTargetDb] = useState('');

  const isConnected = activeProfileId && (
    statuses[activeProfileId]?.state === 'connected' ||
    statuses[activeProfileId]?.state === 'authenticated'
  );

  const loadDatabases = useCallback(async () => {
    if (!activeProfileId || !isConnected) return;
    setDbListLoading(true);
    try {
      const { response } = await request<{ databases?: string[] }>({ action: 'listDatabases' });
      const dbs: Record<string, string[]> = {};
      for (const db of (response.databases ?? [])) dbs[db] = [];
      setDatabases(dbs);
    } catch (e) {
      notify({ type: 'error', title: 'Failed to load databases', message: String(e) });
    } finally {
      setDbListLoading(false);
    }
  }, [activeProfileId, isConnected]);

  const loadCollections = useCallback(async (db: string) => {
    if (!activeProfileId) return;
    try {
      const { response } = await request<{ collections?: string[] }>(
        { action: 'listCollections' },
        { profileId: activeProfileId }
      );
      const cols = response.collections ?? [];
      setDatabases({ ...databases, [db]: cols });
    } catch { /* silent */ }
  }, [activeProfileId, databases]);

  const handleDbClick = async (db: string) => {
    toggleDbExpanded(db);
    setActiveDatabase(db);
    if (!expandedDbs.has(db)) {
      await loadCollections(db);
    }
  };

  const handleCollectionClick = (db: string, col: string) => {
    setActiveDatabase(db);
    setActiveCollection(col);
    navigateTo('data', db, col);
  };

  const activeProfile = profiles.find(p => p.id === activeProfileId);
  const status = activeProfileId ? statuses[activeProfileId] : null;

  return (
    <aside className="w-56 flex-shrink-0 flex flex-col bg-surface-800 border-r border-surface-500/30 overflow-hidden">
      {/* Connection badge */}
      <div className="px-3 py-2.5 border-b border-surface-500/30 flex items-center gap-2">
        <div className={clsx('status-dot flex-shrink-0', {
          'status-connected': status?.state === 'connected',
          'status-authenticated': status?.state === 'authenticated',
          'status-connecting': status?.state === 'connecting',
          'status-disconnected': !status || status.state === 'disconnected',
          'status-error': status?.state === 'error',
        })} />
        <div className="flex-1 min-w-0">
          <p className="text-xs font-medium text-surface-100 truncate">
            {activeProfile?.name ?? 'No connection'}
          </p>
          {activeDatabase && (
            <p className="text-2xs text-surface-400 truncate">{activeDatabase}</p>
          )}
        </div>
        <button
          onClick={() => setSidebarCollapsed(true)}
          className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-surface-300 transition-colors"
          title="Collapse sidebar"
        >
          <PanelLeft size={12} />
        </button>
      </div>

      {/* Database Explorer */}
      {isConnected && (
        <div className="flex-shrink-0 border-b border-surface-500/30">
          <div className="px-2 py-1.5 flex items-center justify-between">
            <span className="section-header">Databases</span>
            <div className="flex gap-0.5">
              <button
                onClick={loadDatabases}
                className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-surface-300 transition-colors"
                title="Refresh databases"
                disabled={dbListLoading}
              >
                <RefreshCw size={11} className={dbListLoading ? 'animate-spin' : ''} />
              </button>
              <button
                onClick={() => setCreateDbOpen(true)}
                className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-surface-300 transition-colors"
                title="Create database"
              >
                <Plus size={11} />
              </button>
            </div>
          </div>

          <div className="max-h-48 overflow-y-auto px-1 pb-1">
            {Object.keys(databases).length === 0 && !dbListLoading && (
              <p className="text-2xs text-surface-500 px-2 py-2">No databases. Click + to create.</p>
            )}
            {Object.entries(databases).map(([db, cols]) => (
              <div key={db}>
                {/* Database row */}
                <div
                  className={clsx(
                    'nav-item justify-between group',
                    activeDatabase === db && 'bg-surface-600/40 text-surface-100',
                  )}
                  onClick={() => handleDbClick(db)}
                >
                  <div className="flex items-center gap-1.5 min-w-0">
                    {expandedDbs.has(db) ? <ChevronDown size={10} /> : <ChevronRight size={10} />}
                    <Database size={11} className="flex-shrink-0 text-pacific-400" />
                    <span className="truncate text-xs">{db}</span>
                  </div>
                  <button
                    className="opacity-0 group-hover:opacity-100 p-0.5 hover:bg-surface-500 rounded"
                    onClick={(e) => { e.stopPropagation(); setTargetDb(db); setCreateCollOpen(true); }}
                    title="Create collection"
                  >
                    <Plus size={10} />
                  </button>
                </div>

                {/* Collections */}
                {expandedDbs.has(db) && cols.map(col => (
                  <div
                    key={col}
                    className={clsx(
                      'nav-item pl-6 text-2xs',
                      activeDatabase === db && activeCollection === col && 'active',
                    )}
                    onClick={() => handleCollectionClick(db, col)}
                    title={col}
                  >
                    <Layers size={10} className="flex-shrink-0 text-surface-400" />
                    <span className="truncate">{col}</span>
                  </div>
                ))}
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Navigation */}
      <nav className="flex-1 overflow-y-auto py-2 px-1">
        {NAV_SECTIONS.map(section => (
          <div key={section.label} className="mb-3">
            <p className="section-header">{section.label}</p>
            {section.items.map(item => {
              const Icon = item.icon;
              const isActive = activeView === item.id;
              return (
                <button
                  key={item.id}
                  id={`nav-${item.id}`}
                  onClick={() => navigateTo(item.id as ActiveView)}
                  className={clsx('nav-item w-full', isActive && 'active')}
                  disabled={!isConnected && !['overview'].includes(item.id)}
                >
                  <Icon size={13} className="flex-shrink-0" />
                  <span className="truncate">{item.label}</span>
                </button>
              );
            })}
          </div>
        ))}
      </nav>

      {/* Bottom actions */}
      <div className="p-2 border-t border-surface-500/30">
        <button
          onClick={() => navigateTo('overview' as ActiveView)}
          className="nav-item w-full"
          id="nav-connections"
        >
          <Server size={13} />
          <span>Connections</span>
        </button>
        <button className="nav-item w-full" id="nav-settings">
          <Settings size={13} />
          <span>Settings</span>
        </button>
      </div>

      {/* Dialogs */}
      <CreateDatabaseDialog
        open={createDbOpen}
        onClose={() => setCreateDbOpen(false)}
        onCreated={loadDatabases}
      />
      <CreateCollectionDialog
        open={createCollOpen}
        database={targetDb}
        onClose={() => setCreateCollOpen(false)}
        onCreated={() => loadCollections(targetDb)}
      />
    </aside>
  );
}
