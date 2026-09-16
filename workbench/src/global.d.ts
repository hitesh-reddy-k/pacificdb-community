/**
 * Global type declarations — exposes the preload IPC bridge as window.pacific
 */

import type { PacificAPI } from '../electron/preload';

declare global {
  interface Window {
    pacific: PacificAPI;
  }
}

export {};
