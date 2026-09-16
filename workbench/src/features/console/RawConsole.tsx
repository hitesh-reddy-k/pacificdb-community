import { useState, useCallback } from 'react';
import { Terminal, Play, Trash2, Copy } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { JsonEditor } from '../../components/ui/JsonEditor';
import { clsx } from 'clsx';

interface ConsoleLine {
  type: 'request' | 'response' | 'error';
  content: string;
  durationMs?: number;
  timestamp: Date;
}

const TEMPLATE = `{
  "action": "ping"
}`;

export function RawConsole() {
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();
  const [input, setInput] = useState(TEMPLATE);
  const [history, setHistory] = useState<ConsoleLine[]>([]);
  const [running, setRunning] = useState(false);

  const push = (line: ConsoleLine) => setHistory(h => [...h, line]);

  const run = useCallback(async () => {
    let parsed: Record<string, unknown>;
    try { parsed = JSON.parse(input); }
    catch (e) { notify({ type: 'error', title: 'Invalid JSON', message: String(e) }); return; }

    push({ type: 'request', content: JSON.stringify(parsed, null, 2), timestamp: new Date() });
    setRunning(true);
    try {
      const { response, durationMs } = await request(parsed);
      push({ type: 'response', content: JSON.stringify(response, null, 2), durationMs, timestamp: new Date() });
    } catch (e) {
      push({ type: 'error', content: String(e), timestamp: new Date() });
    } finally {
      setRunning(false);
    }
  }, [input, request]);

  const copy = (content: string) => {
    navigator.clipboard.writeText(content);
    notify({ type: 'success', title: 'Copied', duration: 2000 });
  };

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <Terminal size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Raw Protocol Console</span>
        <span className="badge-gray ml-1 text-2xs">Direct TCP</span>
        <div className="flex-1" />
        <Button variant="ghost" size="xs" icon={<Trash2 size={11} />} onClick={() => setHistory([])}>Clear</Button>
        <Button variant="primary" size="sm" icon={<Play size={12} />} onClick={run} loading={running} id="console-run-btn">
          Send
        </Button>
      </div>

      <div className="flex-1 flex overflow-hidden">
        {/* Input */}
        <div className="w-1/2 flex flex-col border-r border-surface-500/30">
          <p className="text-2xs text-surface-500 px-3 py-1.5 border-b border-surface-500/20">REQUEST JSON</p>
          <div className="flex-1">
            <JsonEditor value={input} onChange={setInput} placeholder="Enter PacificDB request JSON…" />
          </div>
        </div>

        {/* Output */}
        <div className="w-1/2 flex flex-col overflow-auto">
          <p className="text-2xs text-surface-500 px-3 py-1.5 border-b border-surface-500/20 flex-shrink-0">RESPONSE HISTORY</p>
          <div className="flex-1 overflow-auto p-2 space-y-2 font-mono text-xs">
            {history.length === 0 && (
              <p className="text-surface-600 p-2">Send a request to see the response here.</p>
            )}
            {history.map((line, i) => (
              <div
                key={i}
                className={clsx(
                  'rounded p-2.5 relative group',
                  line.type === 'request' && 'bg-surface-700/50 border border-surface-500/20',
                  line.type === 'response' && 'bg-pacific-500/5 border border-pacific-500/20',
                  line.type === 'error' && 'bg-danger/5 border border-danger/20',
                )}
              >
                <div className="flex items-center gap-2 mb-1">
                  <span className={clsx('text-2xs font-semibold', {
                    'text-surface-400': line.type === 'request',
                    'text-pacific-400': line.type === 'response',
                    'text-danger': line.type === 'error',
                  })}>
                    {line.type.toUpperCase()}
                    {line.durationMs !== undefined && ` · ${line.durationMs}ms`}
                  </span>
                  <span className="text-2xs text-surface-600">{line.timestamp.toLocaleTimeString()}</span>
                </div>
                <pre className="whitespace-pre-wrap selectable text-surface-200 overflow-auto max-h-48">
                  {line.content}
                </pre>
                <button
                  onClick={() => copy(line.content)}
                  className="absolute top-2 right-2 p-1 rounded opacity-0 group-hover:opacity-100 hover:bg-surface-600 text-surface-400 transition-all"
                  title="Copy"
                >
                  <Copy size={10} />
                </button>
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
