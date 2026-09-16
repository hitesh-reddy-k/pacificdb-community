import { type ReactNode, useEffect, useRef } from 'react';
import { createPortal } from 'react-dom';
import { X } from 'lucide-react';
import { clsx } from 'clsx';
import { motion, AnimatePresence } from 'framer-motion';

interface DialogProps {
  open: boolean;
  onClose: () => void;
  title: string;
  description?: string;
  children: ReactNode;
  footer?: ReactNode;
  width?: 'sm' | 'md' | 'lg' | 'xl';
  id?: string;
}

const WIDTHS = {
  sm: 'max-w-sm',
  md: 'max-w-md',
  lg: 'max-w-lg',
  xl: 'max-w-2xl',
};

export function Dialog({ open, onClose, title, description, children, footer, width = 'md', id }: DialogProps) {
  const overlayRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const handler = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    if (open) window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [open, onClose]);

  return createPortal(
    <AnimatePresence>
      {open && (
        <motion.div
          ref={overlayRef}
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.15 }}
          className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 backdrop-blur-sm"
          onClick={(e) => { if (e.target === overlayRef.current) onClose(); }}
        >
          <motion.div
            initial={{ opacity: 0, scale: 0.96, y: 8 }}
            animate={{ opacity: 1, scale: 1, y: 0 }}
            exit={{ opacity: 0, scale: 0.96, y: 8 }}
            transition={{ duration: 0.15, ease: 'easeOut' }}
            className={clsx(
              'w-full bg-surface-800 border border-surface-500/40 rounded-xl shadow-dialog',
              'flex flex-col overflow-hidden',
              WIDTHS[width],
            )}
            id={id}
          >
            {/* Header */}
            <div className="flex items-start justify-between px-5 py-4 border-b border-surface-500/30">
              <div>
                <h2 className="text-md font-semibold text-surface-50">{title}</h2>
                {description && <p className="text-xs text-surface-400 mt-0.5">{description}</p>}
              </div>
              <button
                onClick={onClose}
                className="p-1 rounded hover:bg-surface-600 text-surface-400 hover:text-surface-100 transition-colors ml-4 flex-shrink-0"
                aria-label="Close dialog"
              >
                <X size={14} />
              </button>
            </div>

            {/* Body */}
            <div className="p-5 overflow-y-auto flex-1">{children}</div>

            {/* Footer */}
            {footer && (
              <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-surface-500/30 bg-surface-900/40">
                {footer}
              </div>
            )}
          </motion.div>
        </motion.div>
      )}
    </AnimatePresence>,
    document.body,
  );
}

// ─── Confirm Dialog ───────────────────────────────────────────────────────────

interface ConfirmDialogProps {
  open: boolean;
  onClose: () => void;
  onConfirm: () => void;
  title: string;
  message: ReactNode;
  confirmLabel?: string;
  variant?: 'danger' | 'warning';
  loading?: boolean;
  id?: string;
}

export function ConfirmDialog({
  open, onClose, onConfirm, title, message,
  confirmLabel = 'Confirm', variant = 'danger', loading = false, id,
}: ConfirmDialogProps) {
  return (
    <Dialog
      open={open}
      onClose={onClose}
      title={title}
      width="sm"
      id={id}
      footer={
        <>
          <button onClick={onClose} disabled={loading} className="btn-outline text-sm px-3 py-1.5">
            Cancel
          </button>
          <button
            onClick={onConfirm}
            disabled={loading}
            className={clsx(
              'btn text-sm px-3 py-1.5',
              variant === 'danger'
                ? 'bg-danger text-white hover:bg-danger/90 disabled:opacity-50'
                : 'bg-warning text-black hover:bg-warning/90 disabled:opacity-50',
            )}
          >
            {loading ? 'Processing…' : confirmLabel}
          </button>
        </>
      }
    >
      <p className="text-sm text-surface-200 leading-relaxed">{message}</p>
    </Dialog>
  );
}
