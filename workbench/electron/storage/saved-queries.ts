import * as fs from 'fs';
import * as crypto from 'crypto';

export interface SavedQuery {
  id: string;
  name: string;
  description?: string;
  tags: string[];
  folder?: string;
  profileId?: string;
  database?: string;
  collection?: string;
  action: string;
  request: Record<string, unknown>;
  createdAt: string;
  updatedAt: string;
}

export class SavedQueriesStore {
  private queries: SavedQuery[] = [];

  constructor(private readonly filePath: string) {
    try {
      const raw = fs.readFileSync(filePath, 'utf8');
      this.queries = JSON.parse(raw) as SavedQuery[];
    } catch { this.queries = []; }
  }

  private persist() {
    try {
      const tmp = this.filePath + '.tmp';
      fs.writeFileSync(tmp, JSON.stringify(this.queries, null, 2), { mode: 0o600 });
      fs.renameSync(tmp, this.filePath);
    } catch { /* best effort */ }
  }

  list(): SavedQuery[] { return [...this.queries]; }

  save(query: Omit<SavedQuery, 'id' | 'createdAt' | 'updatedAt'>): SavedQuery {
    const now = new Date().toISOString();
    const saved: SavedQuery = { ...query, id: crypto.randomUUID(), createdAt: now, updatedAt: now };
    this.queries.unshift(saved);
    this.persist();
    return saved;
  }

  update(id: string, updates: Partial<SavedQuery>): SavedQuery {
    const idx = this.queries.findIndex(q => q.id === id);
    if (idx < 0) throw new Error(`Saved query not found: ${id}`);
    this.queries[idx] = { ...this.queries[idx], ...updates, updatedAt: new Date().toISOString() };
    this.persist();
    return this.queries[idx];
  }

  delete(id: string): void {
    this.queries = this.queries.filter(q => q.id !== id);
    this.persist();
  }
}
