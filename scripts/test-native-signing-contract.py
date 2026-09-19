#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")


def require(*needles: str) -> None:
    for needle in needles:
        if needle not in WORKFLOW:
            raise AssertionError(f"release workflow must contain {needle!r}")


require(
    "WINDOWS_CERTIFICATE_BASE64",
    "WINDOWS_CERTIFICATE_PASSWORD",
    "Invoke-CodeSigning",
    'Get-AuthenticodeSignature "$bin\\pacificdb.exe"',
    'Get-AuthenticodeSignature "$bin\\db_engine.exe"',
    "windows_signing",
    "Remove-Item $certPath -Force -ErrorAction SilentlyContinue",
    "MACOS_INSTALLER_CERTIFICATE_BASE64",
    "MACOS_INSTALLER_CERTIFICATE_PASSWORD",
    "MACOS_INSTALLER_IDENTITY",
    "MACOS_KEYCHAIN_PASSWORD",
    "APPLE_NOTARY_ID",
    "APPLE_TEAM_ID",
    "APPLE_APP_SPECIFIC_PASSWORD",
    "cleanup_macos_signing()",
    "trap cleanup_macos_signing EXIT",
    "security create-keychain",
    "security set-key-partition-list",
    "productsign --sign",
    "xcrun notarytool submit",
    "--wait --output-format json",
    "xcrun stapler staple",
    "xcrun stapler validate",
    "spctl --assess --type install",
    "macos_signing",
    '"NOT_APPLICABLE"',
)

if "secrets: inherit" in WORKFLOW:
    raise AssertionError("release validation must not inherit ambient secrets")

print("NATIVE_SIGNING_CONTRACT_PASS")
