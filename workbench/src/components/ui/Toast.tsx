import { useEffect } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { CheckCircle2, XCircle, AlertTriangle, Info, X } from 'lucide-react';
import { useWorkspaceStore } from '../../stores/workspace-store';

const ICONS = {
  success: CheckCircle2,
  error: XCircle,
  warning: AlertTriangle,
  info: Info,
};

const COLORS = {
  success: 'border-success/40 bg-success/10 text-success',
  error: 'border-danger/40 bg-danger/10 text-danger',
  warning: 'border-warning/40 bg-warning/10 text-warning',
  info: 'border-pacific-500/40 bg-pacific-500/10 text-pacific-300',
};

export function ToastContainer() {
  const notifications = useWorkspaceStore(s => s.notifications);
  const dismiss = useWorkspaceStore(s => s.dismissNotification);

  return (
    <div className="fixed bottom-4 right-4 z-50 flex flex-col gap-2 w-80 pointer-events-none">
      <AnimatePresence>
        {notifications.map(n => {
          const Icon = ICONS[n.type];
          return (
            <motion.div
              key={n.id}
              initial={{ opacity: 0, x: 40, scale: 0.96 }}
              animate={{ opacity: 1, x: 0, scale: 1 }}
              exit={{ opacity: 0, x: 40, scale: 0.96 }}
              transition={{ duration: 0.2, ease: 'easeOut' }}
              className={`pointer-events-auto flex items-start gap-3 px-4 py-3 rounded-lg border
                         bg-surface-800 border-surface-500/40 shadow-dialog ${COLORS[n.type]}`}
            >
              <Icon size={16} className="flex-shrink-0 mt-0.5" />
              <div className="flex-1 min-w-0">
                <p className="text-sm font-medium text-surface-100 leading-tight">{n.title}</p>
                {n.message && (
                  <p className="text-xs text-surface-300 mt-0.5 leading-relaxed break-words">{n.message}</p>
                )}
              </div>
              <button
                onClick={() => dismiss(n.id)}
                className="flex-shrink-0 p-0.5 rounded hover:bg-surface-600 text-surface-400 hover:text-surface-200 transition-colors"
                aria-label="Dismiss notification"
              >
                <X size={12} />
              </button>
            </motion.div>
          );
        })}
      </AnimatePresence>
    </div>
  );
}
