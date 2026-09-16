import { lazy, Suspense } from 'react';
import { useWorkspaceStore } from '../stores/workspace-store';
import { SkeletonBlock } from '../components/ui/EmptyState';

// Lazy-load all workspace pages for fast startup
const WelcomePage        = lazy(() => import('../features/connections/WelcomePage').then(m => ({ default: m.WelcomePage })));
const OverviewPage       = lazy(() => import('../features/overview/OverviewPage').then(m => ({ default: m.OverviewPage })));
const DataBrowser        = lazy(() => import('../features/data/DataBrowser').then(m => ({ default: m.DataBrowser })));
const QueryWorkbench     = lazy(() => import('../features/query/QueryWorkbench').then(m => ({ default: m.QueryWorkbench })));
const AggregationBuilder = lazy(() => import('../features/query/AggregationBuilder').then(m => ({ default: m.AggregationBuilder })));
const IndexManager       = lazy(() => import('../features/indexes/IndexManager').then(m => ({ default: m.IndexManager })));
const VectorSearch       = lazy(() => import('../features/vectors/VectorSearch').then(m => ({ default: m.VectorSearch })));
const MediaManager       = lazy(() => import('../features/media/MediaManager').then(m => ({ default: m.MediaManager })));
const BackupManager      = lazy(() => import('../features/backups/BackupManager').then(m => ({ default: m.BackupManager })));
const SecurityPage       = lazy(() => import('../features/security/SecurityPage').then(m => ({ default: m.SecurityPage })));
const ClusterDashboard   = lazy(() => import('../features/cluster/ClusterDashboard').then(m => ({ default: m.ClusterDashboard })));
const MetricsDashboard   = lazy(() => import('../features/metrics/MetricsDashboard').then(m => ({ default: m.MetricsDashboard })));
const NLQPanel           = lazy(() => import('../features/nlq/NLQPanel').then(m => ({ default: m.NLQPanel })));
const RawConsole         = lazy(() => import('../features/console/RawConsole').then(m => ({ default: m.RawConsole })));
const QueryHistory       = lazy(() => import('../features/query/QueryHistory').then(m => ({ default: m.QueryHistory })));
const SavedQueries       = lazy(() => import('../features/query/SavedQueries').then(m => ({ default: m.SavedQueries })));

function Loading() {
  return <SkeletonBlock rows={6} className="p-6" />;
}

export function WorkspaceArea() {
  const activeView = useWorkspaceStore(s => s.activeView);

  const VIEW_MAP: Record<string, JSX.Element> = {
    welcome:   <WelcomePage />,
    overview:  <OverviewPage />,
    data:      <DataBrowser />,
    query:     <QueryWorkbench />,
    aggregate: <AggregationBuilder />,
    indexes:   <IndexManager />,
    vectors:   <VectorSearch />,
    media:     <MediaManager />,
    backups:   <BackupManager />,
    security:  <SecurityPage />,
    cluster:   <ClusterDashboard />,
    metrics:   <MetricsDashboard />,
    nlq:       <NLQPanel />,
    console:   <RawConsole />,
    history:   <QueryHistory />,
    saved:     <SavedQueries />,
  };

  return (
    <main className="flex-1 overflow-hidden bg-surface-900">
      <Suspense fallback={<Loading />}>
        {VIEW_MAP[activeView] ?? <WelcomePage />}
      </Suspense>
    </main>
  );
}
