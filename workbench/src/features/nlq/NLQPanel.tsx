import { useState, useCallback } from 'react';
import { Brain, Play, AlertCircle, Lightbulb } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';

const EXAMPLES = [
  'Find all users older than 30',
  'Count all documents in orders',
  'Find products where price is greater than 100',
  'List all recent documents',
];

export function NLQPanel() {
  const notify = useWorkspaceStore(s => s.notify);
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const { request } = usePacific();

  const [question, setQuestion] = useState('');
  const [compiled, setCompiled] = useState<Record<string, unknown> | null>(null);
  const [compileError, setCompileError] = useState('');
  const [compiling, setCompiling] = useState(false);
  const [result, setResult] = useState<unknown>(null);
  const [running, setRunning] = useState(false);

  const compile = useCallback(async () => {
    if (!question.trim()) return;
    setCompiling(true);
    setCompileError('');
    setCompiled(null);
    setResult(null);
    try {
      const res = await window.pacific.nlq.compile(question, { collection: activeCollection });
      if (res.success) {
        setCompiled(res.result as Record<string, unknown>);
      } else {
        setCompileError(res.error ?? 'Compilation failed');
      }
    } catch (e) {
      setCompileError(String(e));
    } finally {
      setCompiling(false);
    }
  }, [question, activeCollection]);

  const runQuery = useCallback(async () => {
    if (!compiled) return;
    setRunning(true);
    try {
      const { response } = await request(compiled);
      setResult(response);
    } catch (e) {
      notify({ type: 'error', title: 'Query failed', message: String(e) });
    } finally {
      setRunning(false);
    }
  }, [compiled, request]);

  return (
    <div className="h-full overflow-auto p-6 space-y-6">
      <div className="flex items-center gap-2">
        <Brain size={18} className="text-purple-400" />
        <h1 className="text-lg font-bold text-surface-50">Natural Language Query</h1>
        <span className="badge-gray text-2xs">Offline · Deterministic</span>
      </div>

      <div className="bg-surface-700/50 border border-surface-500/30 rounded-lg p-4 text-xs text-surface-400 flex items-start gap-2">
        <Lightbulb size={12} className="text-warning flex-shrink-0 mt-0.5" />
        <p>
          This compiler runs <strong className="text-surface-300">offline and deterministically</strong> using the built-in
          PacificDB NLQ engine. It does not use AI or make external calls.
          It converts natural language descriptions into PacificDB request JSON.
        </p>
      </div>

      <div className="flex flex-col gap-3">
        <div>
          <label className="text-xs font-medium text-surface-200 block mb-1">Question</label>
          <div className="flex gap-2">
            <textarea
              id="nlq-input"
              value={question}
              onChange={e => setQuestion(e.target.value)}
              onKeyDown={e => { if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') compile(); }}
              className="input flex-1 h-20 resize-none selectable"
              placeholder="Find all users where age is greater than 25…"
            />
          </div>
        </div>
        <div className="flex items-center gap-2">
          <Button variant="primary" icon={<Brain size={12} />} onClick={compile} loading={compiling} id="nlq-compile-btn">
            Compile Query
          </Button>
          {compiled && (
            <Button variant="ghost" size="sm" icon={<Play size={12} />} onClick={runQuery} loading={running} id="nlq-run-btn">
              Run Query
            </Button>
          )}
        </div>
      </div>

      {/* Examples */}
      <div>
        <p className="section-header mb-2">Examples</p>
        <div className="flex flex-wrap gap-2">
          {EXAMPLES.map(ex => (
            <button
              key={ex}
              onClick={() => setQuestion(ex)}
              className="tag hover:bg-surface-500 hover:text-surface-100 transition-colors cursor-pointer"
            >
              {ex}
            </button>
          ))}
        </div>
      </div>

      {/* Compile error */}
      {compileError && (
        <div className="flex items-start gap-2 bg-danger/10 border border-danger/30 rounded-lg p-3 text-xs text-danger">
          <AlertCircle size={13} className="flex-shrink-0 mt-0.5" />
          <div>
            <p className="font-semibold">Compilation error</p>
            <p className="mt-0.5 font-mono selectable">{compileError}</p>
          </div>
        </div>
      )}

      {/* Compiled query */}
      {compiled && (
        <div className="card">
          <p className="text-xs font-semibold text-surface-300 mb-2 flex items-center gap-1.5">
            <Brain size={12} className="text-purple-400" /> Generated Query
          </p>
          <pre className="text-xs text-surface-200 font-mono selectable whitespace-pre-wrap">
            {JSON.stringify(compiled, null, 2)}
          </pre>
        </div>
      )}

      {/* Result */}
      {result !== null && (
        <div className="card">
          <p className="text-xs font-semibold text-surface-300 mb-2">Result</p>
          <pre className="text-xs text-surface-200 font-mono selectable whitespace-pre-wrap">
            {JSON.stringify(result, null, 2)}
          </pre>
        </div>
      )}
    </div>
  );
}
