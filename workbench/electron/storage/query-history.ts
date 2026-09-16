import * as fs from 'fs';
import * as crypto from 'crypto';

export interface HistoryEntry {
  id: string;
  profileId: string;
  database?: string;
  collection?: string;
  action: string;
  request: Record<string, unknown>;
  response?: Record<string, unknown>;
  durationMs: number;
  success: boolean;
  error?: string;
  executedAt: string;
}

const MAX_HISTORY = 500;
const SENSITIVE = /password|token|api.?key/i;

function scrub(obj: Record<string, unknown>): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) {
    result[k] = SENSITIVE.test(k) ? '***' : v;
  }
  return result;
}

export class QueryHistoryStore {
  private history: HistoryEntry[] = [];

  constructor(private readonly filePath: string) {
    try {
      const raw = fs.readFileSync(filePath, 'utf8');
      this.history = JSON.parse(raw) as HistoryEntry[];
    } catch { this.history = []; }
  }

  private persist() {
    try {
      const tmp = this.filePath + '.tmp';
      fs.writeFileSync(tmp, JSON.stringify(this.history.slice(-MAX_HISTORY), null, 2), { mode: 0o600 });
      fs.renameSync(tmp, this.filePath);
    } catch { /* best effort */ }
  }

  add(entry: Omit<HistoryEntry, 'id'>): HistoryEntry {
    const full: HistoryEntry = {
      ...entry,
      id: crypto.randomUUID(),
      request: scrub(entry.request),
    };
    this.history.unshift(full);
    if (this.history.length > MAX_HISTORY) this.history = this.history.slice(0, MAX_HISTORY);
    this.persist();
    return full;
  }

  list(profileId?: string): HistoryEntry[] {
    if (profileId) return this.history.filter(e => e.profileId === profileId);
    return [...this.history];
  }

  delete(id: string): void {
    this.history = this.history.filter(e => e.id !== id);
    this.persist();
  }

  clear(profileId?: string): void {
    if (profileId) this.history = this.history.filter(e => e.profileId !== profileId);
    else this.history = [];
    this.persist();
  }
}
