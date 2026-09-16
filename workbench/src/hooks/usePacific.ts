/**
 * usePacific — typed hook for making PacificDB requests from React components.
 * All requests go through main process IPC → TCP → PacificDB server.
 */

import { useCallback } from 'react';
import { useWorkspaceStore } from '../stores/workspace-store';

export interface RequestResult<T = unknown> {
  response: T;
  durationMs: number;
}

export function usePacific() {
  const activeProfileId = useWorkspaceStore(s => s.activeProfileId);
  const notify = useWorkspaceStore(s => s.notify);

  const request = useCallback(async <T = unknown>(
    command: Record<string, unknown>,
    opts?: { profileId?: string; silent?: boolean }
  ): Promise<RequestResult<T>> => {
    const profileId = opts?.profileId ?? activeProfileId;
    if (!profileId) throw new Error('No active connection');
    const result = await window.pacific.request(profileId, command) as RequestResult<T>;
    return result;
  }, [activeProfileId]);

  const requestSilent = useCallback(async <T = unknown>(
    command: Record<string, unknown>,
    opts?: { profileId?: string }
  ): Promise<T | null> => {
    try {
      const r = await request<T>(command, opts);
      return r.response;
    } catch {
      return null;
    }
  }, [request]);

  const requestWithNotify = useCallback(async <T = unknown>(
    command: Record<string, unknown>,
    opts?: { profileId?: string; successMsg?: string; errorMsg?: string }
  ): Promise<RequestResult<T> | null> => {
    try {
      const result = await request<T>(command, opts);
      if (opts?.successMsg) notify({ type: 'success', title: opts.successMsg });
      return result;
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      notify({ type: 'error', title: opts?.errorMsg ?? 'Request failed', message: msg });
      return null;
    }
  }, [request, notify]);

  return { request, requestSilent, requestWithNotify, activeProfileId };
}
