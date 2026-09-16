/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ['./index.html', './src/**/*.{js,ts,jsx,tsx}'],
  darkMode: 'class',
  theme: {
    extend: {
      colors: {
        // PacificDB Brand Palette
        pacific: {
          50:  '#edf6ff',
          100: '#d6eaff',
          200: '#b5d8ff',
          300: '#82bfff',
          400: '#4699ff',
          500: '#1a72ff',  // Primary brand blue
          600: '#0050f0',
          700: '#003fcb',
          800: '#0034a4',
          900: '#042e82',
          950: '#011d57',
        },
        // Neutral surface tones
        surface: {
          900: '#0d0f12',  // Darkest background
          800: '#13161b',  // Sidebar/panel
          700: '#1a1e26',  // Card/section
          600: '#22272f',  // Hover/input
          500: '#2c3340',  // Border/divider
          400: '#3d4555',  // Muted text
          300: '#5a6378',  // Secondary text
          200: '#8896ab',  // Tertiary text
          100: '#c5cedb',  // Body text
          50:  '#e8ecf0',  // Headings in dark
        },
        // Semantic colors
        success: { DEFAULT: '#22c55e', light: '#dcfce7', dark: '#15803d' },
        warning: { DEFAULT: '#f59e0b', light: '#fef3c7', dark: '#b45309' },
        danger:  { DEFAULT: '#ef4444', light: '#fee2e2', dark: '#b91c1c' },
        info:    { DEFAULT: '#3b82f6', light: '#dbeafe', dark: '#1d4ed8' },
        // Status indicators
        status: {
          connected:      '#22c55e',
          connecting:     '#f59e0b',
          disconnected:   '#6b7280',
          error:          '#ef4444',
          authenticated:  '#6366f1',
        },
      },
      fontFamily: {
        sans: ['Inter', 'system-ui', '-apple-system', 'sans-serif'],
        mono: ['JetBrains Mono', 'Fira Code', 'monospace'],
      },
      fontSize: {
        '2xs': ['10px', '14px'],
        'xs': ['11px', '16px'],
        'sm': ['12px', '18px'],
        'base': ['13px', '20px'],
        'md': ['14px', '20px'],
        'lg': ['16px', '24px'],
        'xl': ['18px', '28px'],
        '2xl': ['20px', '28px'],
        '3xl': ['24px', '32px'],
      },
      spacing: {
        '13': '3.25rem',
        '15': '3.75rem',
        '18': '4.5rem',
        '22': '5.5rem',
      },
      borderRadius: {
        'sm': '3px',
        DEFAULT: '5px',
        'md': '7px',
        'lg': '10px',
        'xl': '14px',
      },
      animation: {
        'fade-in':      'fadeIn 150ms ease-out',
        'slide-in-left': 'slideInLeft 200ms ease-out',
        'pulse-dot':    'pulseDot 2s ease-in-out infinite',
        'spin-slow':    'spin 3s linear infinite',
      },
      keyframes: {
        fadeIn:       { from: { opacity: '0' }, to: { opacity: '1' } },
        slideInLeft:  { from: { transform: 'translateX(-8px)', opacity: '0' }, to: { transform: 'translateX(0)', opacity: '1' } },
        pulseDot:     { '0%, 100%': { opacity: '1' }, '50%': { opacity: '0.4' } },
      },
      boxShadow: {
        'panel':  '0 1px 3px rgba(0,0,0,0.4), 0 1px 2px rgba(0,0,0,0.3)',
        'dialog': '0 20px 60px rgba(0,0,0,0.6), 0 4px 20px rgba(0,0,0,0.4)',
        'menu':   '0 8px 24px rgba(0,0,0,0.4), 0 1px 4px rgba(0,0,0,0.3)',
        'inset':  'inset 0 1px 3px rgba(0,0,0,0.3)',
      },
    },
  },
  plugins: [],
};
