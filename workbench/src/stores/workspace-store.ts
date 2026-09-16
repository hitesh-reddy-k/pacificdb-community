/**
 * Workspace store — tracks the active database/collection context,
 * explorer tree state, notifications, and app-wide UI state.
 */

import { create } from 'zustand';

export type ActiveView =
  | 'welcome'
  | 'overview'
  | 'data'
  | 'query'
  | 'aggregate'
  | 'indexes'
  | 'vectors'
  | 'media'
  | 'backups'
  | 'security'
  | 'cluster'
  | 'metrics'
  | 'nlq'
  | 'console'
  | 'history'
  | 'saved';

export interface Notification {
  id: string;
  type: 'success' | 'error' | 'warning' | 'info';
  title: string;
  message?: string;
  duration?: number; // ms, 0 = persistent
}

interface WorkspaceStore {
  // Active context
  activeProfileId: string | null;
  activeDatabase: string | null;
  activeCollection: string | null;
  activeView: ActiveView;

  // Explorer tree
  expandedDbs: Set<string>;
  expandedCollections: Set<string>;
  databases: Record<string, string[]>; // dbName -> collectionNames
  dbListLoading: boolean;

  // Notifications
  notifications: Notification[];

  // UI state
  sidebarCollapsed: boolean;
  bottomPanelOpen: boolean;
  commandPaletteOpen: boolean;

  // Setters
  setActiveProfile: (id: string | null) => void;
  setActiveDatabase: (db: string | null) => void;
  setActiveCollection: (col: string | null) => void;
  setActiveView: (view: ActiveView) => void;
  navigateTo: (view: ActiveView, db?: string, collection?: string) => void;

  // Explorer
  toggleDbExpanded: (db: string) => void;
  setDatabases: (dbs: Record<string, string[]>) => void;
  addCollection: (db: string, collection: string) => void;
  removeCollection: (db: string, collection: string) => void;
  addDatabase: (db: string) => void;
  removeDatabase: (db: string) => void;
  setDbListLoading: (loading: boolean) => void;

  // Notifications
  notify: (n: Omit<Notification, 'id'>) => void;
  dismissNotification: (id: string) => void;
  clearNotifications: () => void;

  // UI
  setSidebarCollapsed: (v: boolean) => void;
  setBottomPanelOpen: (v: boolean) => void;
  setCommandPaletteOpen: (v: boolean) => void;
}

let notifId = 0;

export const useWorkspaceStore = create<WorkspaceStore>((set, get) => ({
  activeProfileId: null,
  activeDatabase: null,
  activeCollection: null,
  activeView: 'welcome',
  expandedDbs: new Set(),
  expandedCollections: new Set(),
  databases: {},
  dbListLoading: false,
  notifications: [],
  sidebarCollapsed: false,
  bottomPanelOpen: false,
  commandPaletteOpen: false,

  setActiveProfile: (id) => set({ activeProfileId: id, activeDatabase: null, activeCollection: null, databases: {} }),
  setActiveDatabase: (db) => set({ activeDatabase: db, activeCollection: null }),
  setActiveCollection: (col) => set({ activeCollection: col }),
  setActiveView: (view) => set({ activeView: view }),

  navigateTo: (view, db, collection) => {
    set(s => ({
      activeView: view,
      activeDatabase: db ?? s.activeDatabase,
      activeCollection: collection ?? (db !== s.activeDatabase ? null : s.activeCollection),
    }));
  },

  toggleDbExpanded: (db) => set(s => {
    const expanded = new Set(s.expandedDbs);
    if (expanded.has(db)) expanded.delete(db); else expanded.add(db);
    return { expandedDbs: expanded };
  }),

  setDatabases: (dbs) => set({ databases: dbs }),

  addCollection: (db, collection) => set(s => ({
    databases: {
      ...s.databases,
      [db]: [...(s.databases[db] ?? []).filter(c => c !== collection), collection].sort(),
    },
  })),

  removeCollection: (db, collection) => set(s => ({
    databases: { ...s.databases, [db]: (s.databases[db] ?? []).filter(c => c !== collection) },
    activeCollection: s.activeCollection === collection ? null : s.activeCollection,
  })),

  addDatabase: (db) => set(s => ({
    databases: s.databases[db] ? s.databases : { ...s.databases, [db]: [] },
  })),

  removeDatabase: (db) => set(s => {
    const { [db]: _removed, ...rest } = s.databases;
    return {
      databases: rest,
      activeDatabase: s.activeDatabase === db ? null : s.activeDatabase,
      activeCollection: s.activeDatabase === db ? null : s.activeCollection,
    };
  }),

  setDbListLoading: (loading) => set({ dbListLoading: loading }),

  notify: (n) => {
    const id = String(++notifId);
    set(s => ({ notifications: [...s.notifications, { ...n, id }] }));
    if ((n.duration ?? 4000) > 0) {
      setTimeout(() => get().dismissNotification(id), n.duration ?? 4000);
    }
  },

  dismissNotification: (id) => set(s => ({ notifications: s.notifications.filter(n => n.id !== id) })),
  clearNotifications: () => set({ notifications: [] }),

  setSidebarCollapsed: (v) => set({ sidebarCollapsed: v }),
  setBottomPanelOpen: (v) => set({ bottomPanelOpen: v }),
  setCommandPaletteOpen: (v) => set({ commandPaletteOpen: v }),
}));
