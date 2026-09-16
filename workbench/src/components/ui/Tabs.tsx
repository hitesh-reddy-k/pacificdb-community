import { type ReactNode, useState } from 'react';
import { clsx } from 'clsx';

interface Tab {
  id: string;
  label: string;
  icon?: ReactNode;
  badge?: string | number;
  disabled?: boolean;
}

interface TabsProps {
  tabs: Tab[];
  activeTab: string;
  onChange: (id: string) => void;
  children: ReactNode;
  size?: 'sm' | 'md';
}

export function Tabs({ tabs, activeTab, onChange, children, size = 'md' }: TabsProps) {
  return (
    <div className="flex flex-col h-full">
      <div className={clsx(
        'flex border-b border-surface-500/30 flex-shrink-0 bg-surface-900/40',
        size === 'sm' ? 'gap-0' : 'gap-0',
      )}>
        {tabs.map(tab => (
          <button
            key={tab.id}
            id={`tab-${tab.id}`}
            role="tab"
            aria-selected={activeTab === tab.id}
            aria-controls={`tabpanel-${tab.id}`}
            disabled={tab.disabled}
            onClick={() => onChange(tab.id)}
            className={clsx(
              'flex items-center gap-1.5 px-4 py-2.5 text-xs font-medium border-b-2 transition-all duration-150',
              'hover:text-surface-100 disabled:opacity-40 disabled:cursor-not-allowed',
              activeTab === tab.id
                ? 'border-pacific-500 text-pacific-300 bg-pacific-500/5'
                : 'border-transparent text-surface-400 hover:border-surface-400/30',
              size === 'sm' && 'px-3 py-2',
            )}
          >
            {tab.icon}
            {tab.label}
            {tab.badge !== undefined && (
              <span className="ml-1 px-1.5 py-0.5 rounded-full text-2xs bg-surface-600 text-surface-300">
                {tab.badge}
              </span>
            )}
          </button>
        ))}
      </div>
      <div className="flex-1 overflow-hidden">{children}</div>
    </div>
  );
}

interface TabPanelProps {
  id: string;
  activeTab: string;
  children: ReactNode;
  className?: string;
}

export function TabPanel({ id, activeTab, children, className }: TabPanelProps) {
  if (id !== activeTab) return null;
  return (
    <div
      id={`tabpanel-${id}`}
      role="tabpanel"
      aria-labelledby={`tab-${id}`}
      className={clsx('h-full overflow-auto', className)}
    >
      {children}
    </div>
  );
}

// ─── Simple state wrapper ─────────────────────────────────────────────────────
export function useTabs(defaultTab: string) {
  const [activeTab, setActiveTab] = useState(defaultTab);
  return { activeTab, setActiveTab };
}
