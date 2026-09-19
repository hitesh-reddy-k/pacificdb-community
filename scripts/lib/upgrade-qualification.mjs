export function rollbackDecision({ interrupted = false, incompatible = false,
  formatTransition = false, oldArtifactAvailable = false } = {}) {
  if (!oldArtifactAvailable) return 'BLOCKED';
  if (incompatible) return 'REJECTED_SAFE';
  if (formatTransition) return 'RESTORE_REQUIRED';
  if (interrupted) return 'RESUMABLE';
  return 'ROLLBACK_SUPPORTED';
}
