import * as fs from 'fs';
import * as crypto from 'crypto';
import type { ConnectionProfile } from '../protocol/types';

interface SafeStorage {
  isEncryptionAvailable(): boolean;
  encryptString(plainText: string): Buffer;
  decryptString(encrypted: Buffer): string;
}

const CURRENT_VERSION = 1;

interface ProfilesFile {
  version: number;
  profiles: ConnectionProfile[];
}

/**
 * ProfilesStore — persists connection profiles to disk.
 * Sensitive fields (passwords, tokens, api keys) are encrypted with
 * Electron's safeStorage API (uses OS keychain on macOS/Windows).
 */
export class ProfilesStore {
  constructor(
    private readonly filePath: string,
    private readonly safeStorage: SafeStorage,
  ) {}

  private load(): ProfilesFile {
    try {
      const raw = fs.readFileSync(this.filePath, 'utf8');
      const data = JSON.parse(raw) as ProfilesFile;
      if (data.version !== CURRENT_VERSION) return { version: CURRENT_VERSION, profiles: [] };
      return data;
    } catch {
      return { version: CURRENT_VERSION, profiles: [] };
    }
  }

  private save(data: ProfilesFile): void {
    const json = JSON.stringify(data, null, 2);
    const tmp = this.filePath + '.tmp';
    fs.writeFileSync(tmp, json, { mode: 0o600 });
    fs.renameSync(tmp, this.filePath);
  }

  list(): ConnectionProfile[] {
    return this.load().profiles;
  }

  get(id: string): ConnectionProfile | null {
    return this.load().profiles.find(p => p.id === id) ?? null;
  }

  create(profile: Omit<ConnectionProfile, 'id' | 'createdAt'>): ConnectionProfile {
    const data = this.load();
    const newProfile: ConnectionProfile = {
      ...profile,
      id: crypto.randomUUID(),
      createdAt: new Date().toISOString(),
    };
    data.profiles.push(newProfile);
    this.save(data);
    return newProfile;
  }

  update(id: string, updates: Partial<ConnectionProfile>): ConnectionProfile {
    const data = this.load();
    const idx = data.profiles.findIndex(p => p.id === id);
    if (idx < 0) throw new Error(`Profile not found: ${id}`);
    const updated = { ...data.profiles[idx], ...updates };
    data.profiles[idx] = updated;
    this.save(data);
    return updated;
  }

  delete(id: string): void {
    const data = this.load();
    data.profiles = data.profiles.filter(p => p.id !== id);
    this.save(data);
  }

  duplicate(id: string): ConnectionProfile {
    const original = this.get(id);
    if (!original) throw new Error(`Profile not found: ${id}`);
    return this.create({
      ...original,
      name: `${original.name} (copy)`,
      auth: original.auth
        ? { ...original.auth, encryptedPassword: undefined, token: undefined, encryptedToken: undefined }
        : undefined,
    });
  }

  // ── Encrypted field helpers ──────────────────────────────────────────────
  encryptSecret(plainText: string): string {
    if (!this.safeStorage.isEncryptionAvailable()) {
      // Fallback: base64 obfuscation (not secure — warn user)
      return 'b64:' + Buffer.from(plainText).toString('base64');
    }
    const encrypted = this.safeStorage.encryptString(plainText);
    return 'enc:' + encrypted.toString('base64');
  }

  decryptSecret(cipherText: string): string {
    if (!cipherText) return '';
    if (cipherText.startsWith('b64:')) {
      return Buffer.from(cipherText.slice(4), 'base64').toString('utf8');
    }
    if (cipherText.startsWith('enc:')) {
      const buf = Buffer.from(cipherText.slice(4), 'base64');
      return this.safeStorage.decryptString(buf);
    }
    return cipherText;
  }
}
