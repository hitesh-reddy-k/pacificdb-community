/**
 * Connection Profile types for PacificDB Workbench.
 * Profiles store all connection parameters; sensitive fields
 * are encrypted at rest using Electron safeStorage.
 */

export type ConnectionEnv = 'local' | 'dev' | 'staging' | 'production';
export type AuthMode = 'none' | 'password' | 'token' | 'apikey';

export interface TlsConfig {
  enabled: boolean;
  verifyServer: boolean;
  caPath?: string;
  certPath?: string;
  keyPath?: string;
}

export interface AuthConfig {
  mode: AuthMode;
  userId?: string;
  username?: string;
  // password/token/apiKey stored encrypted — never as plain text
  encryptedPassword?: string;
  token?: string;           // populated at runtime after authentication
  encryptedToken?: string;  // stored encrypted for API-key auth
}

export interface AdvancedConfig {
  poolSize?: number;          // connections in TCP pool (1–32), default 4
  requestTimeoutMs?: number;  // per-request timeout, default 30000
  connectTimeoutMs?: number;  // connection timeout, default 10000
}

export interface ConnectionProfile {
  id: string;
  name: string;
  host: string;
  port: number;              // default 9000
  env: ConnectionEnv;
  color?: string;            // user-assigned accent color
  database?: string;         // default selected database
  tls?: TlsConfig;
  auth?: AuthConfig;
  advanced?: AdvancedConfig;
  createdAt: string;         // ISO date
  lastConnectedAt?: string;
  notes?: string;
}

export type ConnectionState =
  | 'disconnected'
  | 'connecting'
  | 'connected'
  | 'authenticating'
  | 'authenticated'
  | 'error';

export interface ConnectionStatus {
  profileId: string;
  state: ConnectionState;
  error?: string;
  connectedAt?: string;
  authenticatedAs?: string;
  serverVersion?: string;
  capabilities?: Record<string, unknown>;
}
