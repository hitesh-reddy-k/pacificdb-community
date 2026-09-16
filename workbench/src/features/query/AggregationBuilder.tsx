import { useState, useCallback } from 'react';
import { Layers, Plus, Trash2, GripVertical, Play } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';
import { usePacific } from '../../hooks/usePacific';
import { Button } from '../../components/ui/Button';
import { Select } from '../../components/ui/Input';
import { JsonEditor } from '../../components/ui/JsonEditor';
import { EmptyState } from '../../components/ui/EmptyState';

type StageType = '$match' | '$project' | '$sort' | '$limit' | '$skip' | '$count' | '$group';

interface Stage {
  id: string;
  type: StageType;
  json: string;
}

const STAGE_DEFAULTS: Record<StageType, object> = {
  '$match':   { 'field': 'value' },
  '$project': { '_id': 0, 'field': 1 },
  '$sort':    { 'field': 1 },
  '$limit':   10,
  '$skip':    0,
  '$count':   'total',
  '$group':   { '_id': '$field', 'count': { '$sum': 1 } },
};

export function AggregationBuilder() {
  const activeCollection = useWorkspaceStore(s => s.activeCollection);
  const notify = useWorkspaceStore(s => s.notify);
  const { request } = usePacific();

  const [stages, setStages] = useState<Stage[]>([]);
  const [result, setResult] = useState<unknown>(null);
  const [running, setRunning] = useState(false);

  const addStage = (type: StageType) => {
    const id = crypto.randomUUID();
    const defaultVal = STAGE_DEFAULTS[type];
    const json = JSON.stringify({ [type]: defaultVal }, null, 2);
    setStages(s => [...s, { id, type, json }]);
  };

  const updateStage = (id: string, json: string) => {
    setStages(s => s.map(st => st.id === id ? { ...st, json } : st));
  };

  const removeStage = (id: string) => {
    setStages(s => s.filter(st => st.id !== id));
  };

  const run = useCallback(async () => {
    if (!activeCollection) { notify({ type: 'warning', title: 'Select a collection first' }); return; }
    const pipeline: unknown[] = [];
    for (const stage of stages) {
      try { pipeline.push(JSON.parse(stage.json)); }
      catch (e) { notify({ type: 'error', title: `Invalid JSON in stage ${stage.type}`, message: String(e) }); return; }
    }
    setRunning(true);
    try {
      const { response } = await request({ action: 'aggregate', collection: activeCollection, pipeline });
      setResult(response);
    } catch (e) {
      notify({ type: 'error', title: 'Aggregation failed', message: String(e) });
    } finally {
      setRunning(false);
    }
  }, [stages, activeCollection, request]);

  return (
    <div className="h-full flex flex-col">
      <div className="flex items-center gap-2 px-4 py-3 border-b border-surface-500/30 bg-surface-800/50 flex-shrink-0">
        <Layers size={14} className="text-pacific-400" />
        <span className="text-sm font-semibold text-surface-100">Aggregation Pipeline</span>
        {activeCollection && <span className="text-xs font-mono text-surface-400">{activeCollection}</span>}
        <div className="flex-1" />
        <Select
          id="add-stage-select"
          value=""
          onChange={e => { if (e.target.value) addStage(e.target.value as StageType); }}
          className="w-36 py-1 text-xs"
        >
          <option value="">+ Add Stage</option>
          {(['$match', '$project', '$sort', '$limit', '$skip', '$count', '$group'] as StageType[]).map(t => (
            <option key={t} value={t}>{t}</option>
          ))}
        </Select>
        <Button variant="primary" size="sm" icon={<Play size={12} />} onClick={run} loading={running} disabled={stages.length === 0} id="run-aggregate-btn">
          Run Pipeline
        </Button>
      </div>

      <div className="flex-1 flex overflow-hidden">
        {/* Stages */}
        <div className="flex-1 overflow-auto p-4 space-y-3">
          {stages.length === 0 ? (
            <EmptyState
              icon={<Layers size={18} />}
              title="No stages"
              description="Add stages to build your aggregation pipeline."
              compact className="py-16"
            />
          ) : stages.map((stage, i) => (
            <div key={stage.id} className="border border-surface-500/30 rounded-lg overflow-hidden">
              <div className="flex items-center gap-2 px-3 py-2 bg-surface-800 border-b border-surface-500/30">
                <GripVertical size={12} className="text-surface-600 cursor-grab" />
                <span className="text-2xs font-semibold text-surface-400">Stage {i + 1}</span>
                <span className="font-mono text-xs text-pacific-300 font-bold">{stage.type}</span>
                <div className="flex-1" />
                <button
                  onClick={() => removeStage(stage.id)}
                  className="p-1 rounded hover:bg-surface-600 text-surface-500 hover:text-danger"
                  title="Remove stage"
                >
                  <Trash2 size={11} />
                </button>
              </div>
              <div className="h-32">
                <JsonEditor value={stage.json} onChange={v => updateStage(stage.id, v)} />
              </div>
            </div>
          ))}
        </div>

        {/* Result panel */}
        {result !== null && (
          <div className="w-80 flex-shrink-0 border-l border-surface-500/30 flex flex-col">
            <p className="text-2xs text-surface-500 uppercase tracking-wide px-3 py-2 border-b border-surface-500/30 flex-shrink-0">Result</p>
            <div className="flex-1 overflow-auto p-3">
              <pre className="text-xs text-surface-200 font-mono selectable whitespace-pre-wrap">
                {JSON.stringify(result, null, 2)}
              </pre>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
