/**
 * Connection store — tracks all connection profiles and their live statuses.
 * The actual TCP connections live in main process; this is UI state only.
 */

import { create } from 'zustand';
import type { ConnectionProfile, ConnectionStatus } from '../../electron/protocol/types';

interface ConnectionStore {
  profiles: ConnectionProfile[];
  statuses: Record<string, ConnectionStatus>;
  activeProfileId: string | null;

  // Profile management (calls main IPC)
  loadProfiles: () => Promise<void>;
  createProfile: (profile: Omit<ConnectionProfile, 'id' | 'createdAt'> & { plainPassword?: string; plainToken?: string }) => Promise<ConnectionProfile>;
  updateProfile: (id: string, updates: Partial<ConnectionProfile> & { plainPassword?: string; plainToken?: string }) => Promise<ConnectionProfile>;
  deleteProfile: (id: string) => Promise<void>;
  duplicateProfile: (id: string) => Promise<ConnectionProfile>;

  // Connection lifecycle
  connect: (profileId: string) => Promise<ConnectionStatus>;
  disconnect: (profileId: string) => Promise<void>;
  testConnection: (profileId: string) => Promise<{ success: boolean; latencyMs: number; error?: string; capabilities?: unknown }>;
  setActive: (profileId: string | null) => void;

  // State from main process pushes
  updateStatus: (status: ConnectionStatus) => void;
  loadAllStatuses: () => Promise<void>;
}

export const useConnectionStore = create<ConnectionStore>((set, get) => ({
  profiles: [],
  statuses: {},
  activeProfileId: null,

  loadProfiles: async () => {
    const profiles = await window.pacific.profiles.list() as ConnectionProfile[];
    set({ profiles });
  },

  createProfile: async (profile) => {
    const created = await window.pacific.profiles.create(profile) as ConnectionProfile;
    set(s => ({ profiles: [...s.profiles, created] }));
    return created;
  },

  updateProfile: async (id, updates) => {
    const updated = await window.pacific.profiles.update(id, updates) as ConnectionProfile;
    set(s => ({ profiles: s.profiles.map(p => p.id === id ? updated : p) }));
    return updated;
  },

  deleteProfile: async (id) => {
    await window.pacific.profiles.delete(id);
    set(s => ({
      profiles: s.profiles.filter(p => p.id !== id),
      statuses: Object.fromEntries(Object.entries(s.statuses).filter(([k]) => k !== id)),
      activeProfileId: s.activeProfileId === id ? null : s.activeProfileId,
    }));
  },

  duplicateProfile: async (id) => {
    const dup = await window.pacific.profiles.duplicate(id) as ConnectionProfile;
    set(s => ({ profiles: [...s.profiles, dup] }));
    return dup;
  },

  connect: async (profileId) => {
    set(s => ({
      statuses: { ...s.statuses, [profileId]: { profileId, state: 'connecting' } },
    }));
    const status = await window.pacific.connection.connect(profileId) as ConnectionStatus;
    set(s => ({ statuses: { ...s.statuses, [profileId]: status } }));
    return status;
  },

  disconnect: async (profileId) => {
    await window.pacific.connection.disconnect(profileId);
    set(s => ({
      statuses: { ...s.statuses, [profileId]: { profileId, state: 'disconnected' } },
      activeProfileId: s.activeProfileId === profileId ? null : s.activeProfileId,
    }));
  },

  testConnection: async (profileId) => {
    return window.pacific.connection.test(profileId) as Promise<{ success: boolean; latencyMs: number; error?: string; capabilities?: unknown }>;
  },

  setActive: (profileId) => set({ activeProfileId: profileId }),

  updateStatus: (status) => {
    set(s => ({ statuses: { ...s.statuses, [status.profileId]: status } }));
  },

  loadAllStatuses: async () => {
    const statuses = await window.pacific.connection.statusAll() as ConnectionStatus[];
    const statusMap: Record<string, ConnectionStatus> = {};
    for (const s of statuses) statusMap[s.profileId] = s;
    set({ statuses: statusMap });
  },
}));
