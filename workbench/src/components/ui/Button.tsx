import { type ReactNode, type ButtonHTMLAttributes } from 'react';
import { clsx } from 'clsx';
import { Loader2 } from 'lucide-react';

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'ghost' | 'danger' | 'outline' | 'success';
  size?: 'xs' | 'sm' | 'md';
  loading?: boolean;
  icon?: ReactNode;
  children?: ReactNode;
}

const VARIANTS = {
  primary: 'btn-primary',
  ghost: 'btn-ghost',
  danger: 'btn-danger',
  outline: 'btn-outline',
  success: 'btn bg-success/10 text-success border border-success/30 hover:bg-success/20 focus:ring-success/30',
};

const SIZES = {
  xs: 'px-2 py-1 text-xs gap-1',
  sm: 'px-2.5 py-1 text-xs',
  md: '',
};

export function Button({
  variant = 'ghost',
  size = 'md',
  loading = false,
  icon,
  children,
  className,
  disabled,
  ...props
}: ButtonProps) {
  return (
    <button
      className={clsx(VARIANTS[variant], SIZES[size], className)}
      disabled={disabled || loading}
      {...props}
    >
      {loading ? <Loader2 size={12} className="animate-spin" /> : icon}
      {children}
    </button>
  );
}
