import { PanelLeft, Plus, Wifi, WifiOff, ChevronDown } from 'lucide-react';
import { useConnectionStore } from '../stores/connection-store';
import { useWorkspaceStore } from '../stores/workspace-store';
import { clsx } from 'clsx';
import { useState } from 'react';

export function TopBar() {
  const profiles = useConnectionStore(s => s.profiles);
  const statuses = useConnectionStore(s => s.statuses);
  const disconnect = useConnectionStore(s => s.disconnect);
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const sidebarCollapsed = useWorkspaceStore(s => s.sidebarCollapsed);
  const setSidebarCollapsed = useWorkspaceStore(s => s.setSidebarCollapsed);
  const setActiveProfile = useWorkspaceStore(s => s.setActiveProfile);
  const navigateTo = useWorkspaceStore(s => s.navigateTo);
  const setCommandPaletteOpen = useWorkspaceStore(s => s.setCommandPaletteOpen);

  const [showConnectionMenu, setShowConnectionMenu] = useState(false);

  const activeProfile = profiles.find(p => p.id === activeProfileId);
  const status = activeProfileId ? statuses[activeProfileId] : null;
  const isConnected = status?.state === 'connected' || status?.state === 'authenticated';

  return (
    <header className="h-10 flex items-center px-3 gap-2 bg-surface-900 border-b border-surface-500/30 flex-shrink-0 drag-region select-none">
      {/* Sidebar toggle */}
      <button
        onClick={() => setSidebarCollapsed(!sidebarCollapsed)}
        className="no-drag p-1.5 rounded hover:bg-surface-700 text-surface-400 hover:text-surface-200 transition-colors"
        title="Toggle sidebar"
        id="toggle-sidebar"
      >
        <PanelLeft size={14} />
      </button>

      {/* Brand */}
      <div className="flex items-center gap-2 no-drag">
        <div className="w-5 h-5 rounded bg-gradient-to-br from-pacific-500 to-pacific-700 flex items-center justify-center">
          <span className="text-white text-2xs font-bold">P</span>
        </div>
        <span className="text-xs font-semibold text-surface-200 tracking-wide">PacificDB Workbench</span>
      </div>

      <div className="flex-1" />

      {/* Connection selector */}
      <div className="relative no-drag">
        <button
          id="connection-selector"
          onClick={() => setShowConnectionMenu(v => !v)}
          className={clsx(
            'flex items-center gap-2 px-3 py-1.5 rounded text-xs font-medium transition-all',
            'border hover:bg-surface-700/50',
            isConnected
              ? 'border-pacific-500/30 bg-pacific-500/5 text-pacific-300'
              : 'border-surface-500/30 bg-surface-800 text-surface-300',
          )}
        >
          <div className={clsx('status-dot', {
            'status-authenticated': status?.state === 'authenticated',
            'status-connected': status?.state === 'connected',
            'status-connecting': status?.state === 'connecting',
            'status-disconnected': !status || status.state === 'disconnected',
            'status-error': status?.state === 'error',
          })} />
          <span className="max-w-32 truncate">
            {activeProfile?.name ?? 'No connection'}
          </span>
          {status?.authenticatedAs && (
            <span className="text-2xs text-surface-400">· {status.authenticatedAs}</span>
          )}
          <ChevronDown size={11} />
        </button>

        {showConnectionMenu && (
          <>
            <div className="fixed inset-0 z-40" onClick={() => setShowConnectionMenu(false)} />
            <div className="absolute right-0 top-full mt-1 z-50 bg-surface-700 border border-surface-500/40 rounded-lg shadow-menu min-w-48 py-1 animate-fade-in">
              <div className="px-3 py-1.5 border-b border-surface-500/30">
                <p className="text-2xs text-surface-400 font-medium uppercase tracking-wide">Connections</p>
              </div>
              {profiles.map(p => {
                const st = statuses[p.id];
                const connected = st?.state === 'connected' || st?.state === 'authenticated';
                return (
                  <button
                    key={p.id}
                    onClick={() => {
                      setActiveProfile(p.id);
                      navigateTo('overview');
                      setShowConnectionMenu(false);
                    }}
                    className={clsx(
                      'w-full flex items-center gap-2 px-3 py-2 text-xs hover:bg-surface-600 transition-colors text-left',
                      p.id === activeProfileId && 'bg-pacific-500/10 text-pacific-300',
                    )}
                  >
                    <div className={clsx('status-dot', {
                      'status-connected': connected,
                      'status-disconnected': !connected,
                    })} />
                    <div className="flex-1 min-w-0">
                      <p className="truncate font-medium text-surface-100">{p.name}</p>
                      <p className="truncate text-surface-400 text-2xs">{p.host}:{p.port}</p>
                    </div>
                    {connected
                      ? <Wifi size={11} className="text-success flex-shrink-0" />
                      : <WifiOff size={11} className="text-surface-500 flex-shrink-0" />}
                  </button>
                );
              })}
              <div className="border-t border-surface-500/30 mt-1 pt-1 px-1">
                <button
                  onClick={() => { navigateTo('overview'); setShowConnectionMenu(false); }}
                  className="w-full flex items-center gap-2 px-2 py-1.5 text-xs text-pacific-400 hover:bg-surface-600 rounded transition-colors"
                  id="manage-connections-btn"
                >
                  <Plus size={11} />
                  Manage connections
                </button>
              </div>
            </div>
          </>
        )}
      </div>

      {/* Disconnect */}
      {isConnected && activeProfileId && (
        <button
          onClick={() => disconnect(activeProfileId)}
          className="no-drag p-1.5 rounded hover:bg-surface-700 text-surface-500 hover:text-danger transition-colors"
          title="Disconnect"
          id="disconnect-btn"
        >
          <WifiOff size={13} />
        </button>
      )}

      {/* Command palette hint */}
      <button
        onClick={() => setCommandPaletteOpen(true)}
        className="no-drag flex items-center gap-1.5 px-2 py-1 rounded border border-surface-500/30 text-surface-400 hover:text-surface-200 hover:bg-surface-700/50 transition-colors text-2xs"
        id="cmd-palette-btn"
        title="Open command palette (Ctrl+K)"
      >
        <kbd className="font-mono">⌘K</kbd>
      </button>
    </header>
  );
}
