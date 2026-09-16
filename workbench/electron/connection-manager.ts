import { EventEmitter } from 'events';
import { BrowserWindow } from 'electron';
import { PacificClient } from './protocol/pacific-client';
import type { ConnectionProfile, ConnectionStatus, ConnectionState } from './protocol/types';

interface ConnectionEntry {
  profile: ConnectionProfile;
  client: PacificClient;
  status: ConnectionStatus;
}

/**
 * ConnectionManager — owns all PacificClient instances keyed by profile ID.
 * Notifies the renderer of state changes via main-window IPC.
 */
export class ConnectionManager extends EventEmitter {
  private connections = new Map<string, ConnectionEntry>();

  private pushStateChange(status: ConnectionStatus) {
    const win = BrowserWindow.getAllWindows()[0];
    if (win && !win.isDestroyed()) {
      win.webContents.send('connection:stateChange', status);
    }
    this.emit('stateChange', status);
  }

  private updateState(
    profileId: string,
    state: ConnectionState,
    extras: Partial<ConnectionStatus> = {},
  ) {
    const entry = this.connections.get(profileId);
    if (!entry) return;
    Object.assign(entry.status, { state, ...extras });
    this.pushStateChange({ ...entry.status });
  }

  async connect(profile: ConnectionProfile): Promise<ConnectionStatus> {
    // Close existing if reconnecting
    const existing = this.connections.get(profile.id);
    if (existing) {
      existing.client.disconnect();
      this.connections.delete(profile.id);
    }

    const client = new PacificClient(profile);
    const status: ConnectionStatus = {
      profileId: profile.id,
      state: 'connecting',
    };
    this.connections.set(profile.id, { profile, client, status });
    this.pushStateChange({ ...status });

    try {
      // Establish TCP connection
      await client.connect();
      this.updateState(profile.id, 'connected', { connectedAt: new Date().toISOString() });

      // Ping to verify server is responding
      await client.ping();

      // If credentials provided, authenticate
      if (profile.auth?.mode === 'password' && profile.auth?.username) {
        this.updateState(profile.id, 'authenticating');
        // Password is injected at connect time from the decrypted profile
        const creds = (profile as ConnectionProfile & { _resolvedPassword?: string });
        const password = creds._resolvedPassword ?? '';
        const result = await client.authenticate(profile.auth.username, password);
        const username = typeof result.username === 'string' ? result.username : profile.auth.username;
        this.updateState(profile.id, 'authenticated', { authenticatedAs: username });
      } else if (profile.auth?.mode === 'token' && profile.auth?.token) {
        client.token = profile.auth.token;
        this.updateState(profile.id, 'authenticated');
      } else if (profile.auth?.mode === 'apikey' && profile.auth?.encryptedToken) {
        // apiKey was already resolved at call site
        const apiKeyEntry = profile as ConnectionProfile & { _resolvedApiKey?: string };
        if (apiKeyEntry._resolvedApiKey) client.token = apiKeyEntry._resolvedApiKey;
        this.updateState(profile.id, 'authenticated');
      } else {
        this.updateState(profile.id, 'connected');
      }

      // Fetch server capabilities
      try {
        const cap = await client.capabilities();
        this.updateState(profile.id, this.connections.get(profile.id)!.status.state, {
          capabilities: cap as Record<string, unknown>,
        });
      } catch { /* capabilities is optional */ }

      return { ...this.connections.get(profile.id)!.status };
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      client.disconnect();
      this.connections.delete(profile.id);
      const errStatus: ConnectionStatus = { profileId: profile.id, state: 'error', error: msg };
      this.pushStateChange(errStatus);
      throw error;
    }
  }

  async disconnect(profileId: string): Promise<void> {
    const entry = this.connections.get(profileId);
    if (!entry) return;
    entry.client.disconnect();
    this.connections.delete(profileId);
    this.pushStateChange({ profileId, state: 'disconnected' });
  }

  async disconnectAll(): Promise<void> {
    const ids = [...this.connections.keys()];
    await Promise.allSettled(ids.map(id => this.disconnect(id)));
  }

  async test(profile: ConnectionProfile): Promise<{ success: boolean; latencyMs: number; error?: string; capabilities?: unknown }> {
    const start = Date.now();
    const testClient = new PacificClient({
      ...profile,
      advanced: { ...profile.advanced, poolSize: 1, requestTimeoutMs: 10000 },
    });
    try {
      await testClient.connect();
      await testClient.ping();
      const cap = await testClient.capabilities().catch(() => null);
      return { success: true, latencyMs: Date.now() - start, capabilities: cap };
    } catch (error) {
      return { success: false, latencyMs: Date.now() - start, error: error instanceof Error ? error.message : String(error) };
    } finally {
      testClient.disconnect();
    }
  }

  getClient(profileId: string): PacificClient | null {
    return this.connections.get(profileId)?.client ?? null;
  }

  getStatus(profileId: string): ConnectionStatus | null {
    const entry = this.connections.get(profileId);
    return entry ? { ...entry.status } : null;
  }

  getAllStatuses(): ConnectionStatus[] {
    return [...this.connections.values()].map(e => ({ ...e.status }));
  }

  isConnected(profileId: string): boolean {
    return this.connections.has(profileId);
  }
}
