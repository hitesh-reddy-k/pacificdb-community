#!/usr/bin/env bash
set -euo pipefail

has_command() { command -v "$1" >/dev/null 2>&1; }
json_bool() { if "$@"; then printf true; else printf false; fi; }

qemu_available=$(json_bool has_command qemu-system-x86_64)
kvm_available=false
[[ -r /dev/kvm && -w /dev/kvm ]] && kvm_available=true
vm_image_configured=false
[[ -n ${PACIFICDB_CERT_VM_IMAGE:-} && -f ${PACIFICDB_CERT_VM_IMAGE:-} ]] && vm_image_configured=true

windows_host_configured=false
[[ -n ${PACIFICDB_WINDOWS_SSH_HOST:-} ]] && windows_host_configured=true
macos_host_configured=false
[[ -n ${PACIFICDB_MACOS_SSH_HOST:-} ]] && macos_host_configured=true
windows_signing_configured=false
[[ -n ${PACIFICDB_WINDOWS_SIGN_CERTIFICATE:-} && -n ${PACIFICDB_WINDOWS_SIGN_PASSWORD_FILE:-} ]] && windows_signing_configured=true
macos_signing_configured=false
[[ -n ${PACIFICDB_MACOS_SIGNING_IDENTITY:-} && -n ${PACIFICDB_MACOS_NOTARY_PROFILE:-} ]] && macos_signing_configured=true

physical_status=BLOCKED
physical_reason="no dedicated physical power-cut and storage-controller harness is configured"
if [[ -n ${PACIFICDB_PHYSICAL_POWER_HARNESS:-} && -x ${PACIFICDB_PHYSICAL_POWER_HARNESS:-} ]]; then
  physical_status=AVAILABLE
  physical_reason="dedicated harness is configured; execution requires its hardware-specific runbook"
fi

vm_status=BLOCKED
vm_reason="no dedicated disposable VM image is configured"
if [[ $qemu_available == true && $kvm_available == true && $vm_image_configured == true ]]; then
  vm_status=AVAILABLE
  vm_reason="QEMU/KVM and a disposable VM image are configured"
fi

windows_status=BLOCKED
windows_reason="no authorized native Windows host is configured"
if [[ $windows_host_configured == true ]]; then
  if ssh -o BatchMode=yes -o ConnectTimeout=5 "$PACIFICDB_WINDOWS_SSH_HOST" 'ver' >/dev/null 2>&1; then
    windows_status=AVAILABLE
    windows_reason="authorized Windows host is reachable"
  else
    windows_reason="configured Windows host is unreachable or not authorized"
  fi
fi

macos_status=BLOCKED
macos_reason="no authorized native macOS host is configured"
if [[ $macos_host_configured == true ]]; then
  if ssh -o BatchMode=yes -o ConnectTimeout=5 "$PACIFICDB_MACOS_SSH_HOST" 'sw_vers' >/dev/null 2>&1; then
    macos_status=AVAILABLE
    macos_reason="authorized macOS host is reachable"
  else
    macos_reason="configured macOS host is unreachable or not authorized"
  fi
fi

cat <<JSON
{
  "physical_power_storage_controller": {"status":"$physical_status","reason":"$physical_reason"},
  "virtual_hard_power": {"status":"$vm_status","reason":"$vm_reason","qemu":$qemu_available,"kvm":$kvm_available,"image":$vm_image_configured},
  "windows_native": {"status":"$windows_status","reason":"$windows_reason"},
  "windows_signing": {"status":"$([[ $windows_signing_configured == true ]] && echo AVAILABLE || echo BLOCKED)","credentials_configured":$windows_signing_configured},
  "macos_native": {"status":"$macos_status","reason":"$macos_reason"},
  "macos_signing_notarization": {"status":"$([[ $macos_signing_configured == true ]] && echo AVAILABLE || echo BLOCKED)","credentials_configured":$macos_signing_configured}
}
JSON
