import { type ReactNode } from 'react';
import { clsx } from 'clsx';

interface EmptyStateProps {
  icon: ReactNode;
  title: string;
  description?: string;
  action?: ReactNode;
  className?: string;
  compact?: boolean;
}

export function EmptyState({ icon, title, description, action, className, compact = false }: EmptyStateProps) {
  return (
    <div className={clsx(
      'flex flex-col items-center justify-center text-center',
      compact ? 'py-8 px-4 gap-2' : 'py-16 px-8 gap-3',
      className,
    )}>
      <div className={clsx(
        'rounded-xl bg-surface-700/50 flex items-center justify-center text-surface-400',
        compact ? 'w-10 h-10' : 'w-14 h-14',
      )}>
        {icon}
      </div>
      <div>
        <p className={clsx('font-semibold text-surface-200', compact ? 'text-sm' : 'text-md')}>{title}</p>
        {description && (
          <p className={clsx('text-surface-400 mt-1', compact ? 'text-xs' : 'text-sm')}>{description}</p>
        )}
      </div>
      {action && <div className="mt-1">{action}</div>}
    </div>
  );
}

// ─── Skeleton loader ──────────────────────────────────────────────────────────

export function SkeletonLine({ width = '100%', height = 14 }: { width?: string | number; height?: number }) {
  return (
    <div
      className="skeleton rounded"
      style={{ width: typeof width === 'number' ? `${width}px` : width, height }}
    />
  );
}

export function SkeletonBlock({ rows = 4, className }: { rows?: number; className?: string }) {
  return (
    <div className={clsx('flex flex-col gap-2 p-4', className)}>
      {Array.from({ length: rows }).map((_, i) => (
        <SkeletonLine key={i} width={`${60 + Math.random() * 30}%`} />
      ))}
    </div>
  );
}
