import { useState, useEffect, useRef, useCallback } from 'react';
import { createPortal } from 'react-dom';
import {
  Search, Database, Layers, Activity, Shield, ArchiveRestore,
  Terminal, History, Bookmark, Brain, Network, Plus, Wifi,
} from 'lucide-react';
import { useWorkspaceStore, type ActiveView } from '../../stores/workspace-store';
import { useConnectionStore } from '../../stores/connection-store';
import { motion, AnimatePresence } from 'framer-motion';
import { clsx } from 'clsx';

interface Command {
  id: string;
  label: string;
  description?: string;
  icon: React.ReactNode;
  action: () => void;
  keywords?: string[];
}

interface Props {
  onClose: () => void;
}

export function CommandPalette({ onClose }: Props) {
  const [query, setQuery] = useState('');
  const [selected, setSelected] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const navigateTo = useWorkspaceStore(s => s.navigateTo);
  const profiles = useConnectionStore(s => s.profiles);
  const statuses = useConnectionStore(s => s.statuses);
  const connect = useConnectionStore(s => s.connect);
  const setActive = useWorkspaceStore(s => s.setActiveProfile);
  const notify = useWorkspaceStore(s => s.notify);

  useEffect(() => {
    inputRef.current?.focus();
  }, []);

  const buildCommands = useCallback((): Command[] => [
    { id: 'nav-overview',  label: 'Go to Overview',        icon: <Activity size={14} />,       action: () => { navigateTo('overview'); onClose(); }, keywords: ['home', 'server', 'status'] },
    { id: 'nav-data',      label: 'Go to Data Browser',    icon: <Database size={14} />,       action: () => { navigateTo('data'); onClose(); } },
    { id: 'nav-query',     label: 'Go to Query Workbench', icon: <Search size={14} />,         action: () => { navigateTo('query'); onClose(); } },
    { id: 'nav-aggregate', label: 'Go to Aggregation',     icon: <Layers size={14} />,         action: () => { navigateTo('aggregate'); onClose(); } },
    { id: 'nav-indexes',   label: 'Go to Indexes',         icon: <Layers size={14} />,         action: () => { navigateTo('indexes'); onClose(); } },
    { id: 'nav-vectors',   label: 'Go to Vector Search',   icon: <Brain size={14} />,          action: () => { navigateTo('vectors'); onClose(); }, keywords: ['ml', 'similarity', 'embedding'] },
    { id: 'nav-media',     label: 'Go to Media',           icon: <ArchiveRestore size={14} />, action: () => { navigateTo('media'); onClose(); }, keywords: ['files', 'upload', 'images'] },
    { id: 'nav-backups',   label: 'Go to Backups',         icon: <ArchiveRestore size={14} />, action: () => { navigateTo('backups'); onClose(); } },
    { id: 'nav-security',  label: 'Go to Security',        icon: <Shield size={14} />,         action: () => { navigateTo('security'); onClose(); }, keywords: ['api keys', 'auth', 'roles'] },
    { id: 'nav-cluster',   label: 'Go to Cluster',         icon: <Network size={14} />,        action: () => { navigateTo('cluster'); onClose(); }, keywords: ['raft', 'nodes'] },
    { id: 'nav-nlq',       label: 'Natural Language Query',icon: <Brain size={14} />,          action: () => { navigateTo('nlq'); onClose(); }, keywords: ['ai', 'english', 'describe'] },
    { id: 'nav-console',   label: 'Go to Raw Console',     icon: <Terminal size={14} />,       action: () => { navigateTo('console'); onClose(); }, keywords: ['raw', 'protocol', 'debug'] },
    { id: 'nav-history',   label: 'Go to Query History',   icon: <History size={14} />,        action: () => { navigateTo('history'); onClose(); } },
    { id: 'nav-saved',     label: 'Go to Saved Queries',   icon: <Bookmark size={14} />,       action: () => { navigateTo('saved'); onClose(); } },
    ...profiles.map(p => ({
      id: `connect-${p.id}`,
      label: `Connect: ${p.name}`,
      description: `${p.host}:${p.port}`,
      icon: <Wifi size={14} />,
      keywords: ['connect', p.host],
      action: async () => {
        setActive(p.id);
        onClose();
        try {
          await connect(p.id);
          navigateTo('overview');
        } catch (e) {
          notify({ type: 'error', title: 'Connection failed', message: String(e) });
        }
      },
    })),
  ], [profiles, navigateTo, connect, setActive, notify, onClose]);

  const commands = buildCommands();
  const filtered = query
    ? commands.filter(c =>
        c.label.toLowerCase().includes(query.toLowerCase()) ||
        c.description?.toLowerCase().includes(query.toLowerCase()) ||
        c.keywords?.some(k => k.toLowerCase().includes(query.toLowerCase()))
      )
    : commands;

  useEffect(() => { setSelected(0); }, [query]);

  const handleKey = (e: React.KeyboardEvent) => {
    if (e.key === 'Escape') { onClose(); return; }
    if (e.key === 'ArrowDown') { e.preventDefault(); setSelected(s => Math.min(s + 1, filtered.length - 1)); }
    if (e.key === 'ArrowUp') { e.preventDefault(); setSelected(s => Math.max(s - 1, 0)); }
    if (e.key === 'Enter' && filtered[selected]) { filtered[selected].action(); }
  };

  return createPortal(
    <div className="fixed inset-0 z-50 flex items-start justify-center pt-24 bg-black/60 backdrop-blur-sm" onClick={onClose}>
      <motion.div
        initial={{ opacity: 0, scale: 0.95, y: -8 }}
        animate={{ opacity: 1, scale: 1, y: 0 }}
        exit={{ opacity: 0, scale: 0.95, y: -8 }}
        transition={{ duration: 0.15 }}
        className="w-full max-w-xl bg-surface-800 border border-surface-500/40 rounded-xl shadow-dialog overflow-hidden"
        onClick={e => e.stopPropagation()}
      >
        {/* Search input */}
        <div className="flex items-center gap-3 px-4 py-3.5 border-b border-surface-500/30">
          <Search size={16} className="text-surface-400 flex-shrink-0" />
          <input
            ref={inputRef}
            value={query}
            onChange={e => setQuery(e.target.value)}
            onKeyDown={handleKey}
            placeholder="Type a command or search…"
            className="flex-1 bg-transparent text-surface-100 placeholder-surface-500 text-sm outline-none"
            id="command-palette-input"
          />
          <kbd className="text-2xs bg-surface-700 border border-surface-500/30 rounded px-1.5 py-0.5 text-surface-400 font-mono">Esc</kbd>
        </div>

        {/* Results */}
        <div className="max-h-80 overflow-y-auto py-1">
          {filtered.length === 0 ? (
            <p className="text-sm text-surface-500 text-center py-8">No results</p>
          ) : filtered.map((cmd, i) => (
            <button
              key={cmd.id}
              id={`cmd-${cmd.id}`}
              onClick={() => cmd.action()}
              onMouseEnter={() => setSelected(i)}
              className={clsx(
                'w-full flex items-center gap-3 px-4 py-2.5 text-left transition-colors',
                i === selected ? 'bg-pacific-500/15 text-pacific-300' : 'text-surface-200 hover:bg-surface-700/40',
              )}
            >
              <span className={clsx('flex-shrink-0', i === selected ? 'text-pacific-400' : 'text-surface-500')}>
                {cmd.icon}
              </span>
              <div className="flex-1 min-w-0">
                <p className="text-sm font-medium truncate">{cmd.label}</p>
                {cmd.description && (
                  <p className="text-2xs text-surface-500 font-mono truncate">{cmd.description}</p>
                )}
              </div>
              {i === selected && (
                <kbd className="text-2xs bg-surface-700 border border-surface-500/30 rounded px-1.5 py-0.5 text-surface-400 font-mono flex-shrink-0">↵</kbd>
              )}
            </button>
          ))}
        </div>
      </motion.div>
    </div>,
    document.body,
  );
}
