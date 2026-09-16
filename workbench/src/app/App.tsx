import { useEffect } from 'react';
import { useConnectionStore } from '../stores/connection-store';
import { useWorkspaceStore } from '../stores/workspace-store';
import { Sidebar } from './Sidebar';
import { TopBar } from './TopBar';
import { WorkspaceArea } from './WorkspaceArea';
import { ToastContainer } from '../components/ui/Toast';
import { CommandPalette } from '../features/command-palette/CommandPalette';

export function App() {
  const loadProfiles = useConnectionStore(s => s.loadProfiles);
  const loadAllStatuses = useConnectionStore(s => s.loadAllStatuses);
  const updateStatus = useConnectionStore(s => s.updateStatus);
  const sidebarCollapsed = useWorkspaceStore(s => s.sidebarCollapsed);
  const commandPaletteOpen = useWorkspaceStore(s => s.commandPaletteOpen);
  const setCommandPaletteOpen = useWorkspaceStore(s => s.setCommandPaletteOpen);

  // Bootstrap on mount
  useEffect(() => {
    loadProfiles();
    loadAllStatuses();

    // Subscribe to connection state changes pushed from main process
    const unsub = window.pacific.connection.onStateChange((event) => {
      updateStatus(event as Parameters<typeof updateStatus>[0]);
    });

    // Keyboard shortcut: Ctrl/Cmd+K → command palette
    const handleKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key === 'k') {
        e.preventDefault();
        setCommandPaletteOpen(true);
      }
    };
    window.addEventListener('keydown', handleKey);

    return () => {
      if (typeof unsub === 'function') unsub();
      window.removeEventListener('keydown', handleKey);
    };
  }, []);

  return (
    <div className="flex flex-col h-full w-full bg-surface-900 overflow-hidden">
      <TopBar />
      <div className="flex flex-1 overflow-hidden">
        {!sidebarCollapsed && <Sidebar />}
        <WorkspaceArea />
      </div>
      <ToastContainer />
      {commandPaletteOpen && <CommandPalette onClose={() => setCommandPaletteOpen(false)} />}
    </div>
  );
}
