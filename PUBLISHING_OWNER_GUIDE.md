# Owner publication process — Pre-release v1.2

Release identity: base `1.2.0`, Git tag `v1.2.0-rc.2`, branch `v1.2`, title
**Pre-release v1.2**. npm/Java version `1.2.0-rc.2`; Python version `1.2.0rc2`.

Account/publisher setup can be completed now. Run publication only after the exact
RC tag exists and release qualification passes. Do not create, move or overwrite
the tag to get past a failed guard. The release manifest records the actual source
commit and scoped tests. This is a prerelease, not stable/production certification.

## 1. PyPI: configure ownership and trusted publishing

1. Sign in to [PyPI](https://pypi.org/) with the account that owns `pacificdb` and
   complete any required two-factor authentication.
2. Open [pacificdb publishing settings](https://pypi.org/manage/project/pacificdb/settings/publishing/).
   If the project exists but you cannot manage it, its owner must grant access.
   A GitHub repository name alone does not grant PyPI ownership.
3. Add a **GitHub Actions** trusted publisher with these exact values:

   | Field | Value |
   | --- | --- |
   | Owner | `hitesh-reddy-k` |
   | Repository | `pacificdb-community` |
   | Workflow filename | `python-publish.yml` |
   | Environment | `pypi` |

   Use the filename alone, not `.github/workflows/python-publish.yml`. If `pacificdb`
   does not yet exist, an authorized owner can instead register a pending publisher
   at [account publishing settings](https://pypi.org/manage/account/publishing/),
   including project name `pacificdb`. Do not register another project's name.
4. Confirm GitHub repository Settings → Environments contains environment `pypi`.
   [Direct environment settings](https://github.com/hitesh-reddy-k/pacificdb-community/settings/environments).
   The workflow requests an OIDC token. No PyPI password/API token belongs in code
   or chat, and this OIDC workflow does not need a `PYPI_TOKEN` secret.
5. Once the validated tag exists, open Actions → **Publish tested Python SDK** →
   **Run workflow**, choose branch `v1.2`, and enter release tag `v1.2.0-rc.2`.
   Alternatively, from an authenticated owner terminal:

   ```sh
   gh workflow run python-publish.yml \
     --repo hitesh-reddy-k/pacificdb-community --ref v1.2 \
     -f release_tag=v1.2.0-rc.2
   gh run list --repo hitesh-reddy-k/pacificdb-community \
     --workflow python-publish.yml --limit 3
   ```

6. Wait for both qualification and publish jobs. `invalid-publisher` means the
   owner/repository/filename/environment mapping does not match, even if GitHub
   issued a valid OIDC token. Fix that mapping; do not bypass the checks.
7. Verify the actual release at [PyPI pacificdb 1.2.0rc2](https://pypi.org/project/pacificdb/1.2.0rc2/).
   A link here is an intended destination, not a claim that it is published.

   ```sh
   python3 -m venv /tmp/pacificdb-pypi-rc-consumer
   /tmp/pacificdb-pypi-rc-consumer/bin/python -m pip install \
     --no-cache-dir 'pacificdb==1.2.0rc2'
   /tmp/pacificdb-pypi-rc-consumer/bin/python -c \
     'import importlib.metadata; print(importlib.metadata.version("pacificdb"))'
   ```

   Windows uses the venv's `Scripts/python.exe`. Run the engine-backed Python
   example from SDK_GUIDE.md too; import/version checks alone do not prove it works.
   Retain the Actions run URL and compare registry file SHA-256 values with its
   tested package manifest. PyPI versions/files cannot be overwritten on retry.

Official references: [existing project publisher](https://docs.pypi.org/trusted-publishers/adding-a-publisher/),
[pending publisher](https://docs.pypi.org/trusted-publishers/creating-a-project-through-oidc/),
[OIDC troubleshooting](https://docs.pypi.org/trusted-publishers/troubleshooting/).

## 2. Maven Central: verify namespace, signing and token

1. Sign in to [Central Publisher Portal](https://central.sonatype.com/).
2. Check **Namespaces**. Your account must be authorized for **`io.pacificdb`**,
   matching `io.pacificdb:pacificdb-client`. Complete Sonatype's ownership/DNS
   verification if required. If you do not own that namespace, stop and report it;
   do not rename the public coordinates or assume GitHub access grants ownership.
3. Open [User Tokens](https://central.sonatype.com/usertoken), generate a Portal
   user token and retain its username/password privately. `CENTRAL_TOKEN` for the
   repository's API uploader is **Base64 of `tokenUsername:tokenPassword`**, not
   your account password, a GitHub token, or an arbitrary raw token string.
4. Use your release-owner GPG signing key. List existing keys locally first:

   ```sh
   gpg --list-secret-keys --keyid-format LONG
   gpg --fingerprint YOUR_KEY_ID
   ```

   The repository requires the full **40-hex-character primary fingerprint**, not
   a short key ID. If you have no key, create a passphrase-protected signing key
   with `gpg --full-generate-key` following Sonatype's current signing guidance.
   Back it up privately; do not replace an existing release identity unnecessarily.
5. Publish only the **public** signing key to the public key server recommended by
   Sonatype, for example:

   ```sh
   gpg --keyserver hkps://keyserver.ubuntu.com --send-keys YOUR_FULL_FINGERPRINT
   ```

   Never upload the private key to a key server or send it in chat/issues.
6. GitHub repository Settings → Environments → **maven-central** → add:

   | Type | Name | Content |
   | --- | --- | --- |
   | Secret | `CENTRAL_TOKEN` | Base64 Portal token username:password |
   | Secret | `GPG_PRIVATE_KEY` | Full ASCII-armored private signing key block |
   | Secret | `GPG_PASSPHRASE` | Signing key passphrase |
   | Variable | `GPG_FINGERPRINT` | Full primary fingerprint, 40 hex characters |

   The private key must include `BEGIN PGP PRIVATE KEY BLOCK` and its real
   newlines. The public key is not sufficient for signing. Export to a private
   temporary directory **outside the repository**, then import into the GitHub
   secret directly (replace the public fingerprint placeholder):

   ```sh
   signing_dir=$(mktemp -d)
   chmod 700 "$signing_dir"
   gpg --armor --export-secret-keys YOUR_FULL_FINGERPRINT > "$signing_dir/key.asc"
   chmod 600 "$signing_dir/key.asc"
   gh secret set GPG_PRIVATE_KEY --env maven-central \
     --repo hitesh-reddy-k/pacificdb-community < "$signing_dir/key.asc"
   gh secret set GPG_PASSPHRASE --env maven-central \
     --repo hitesh-reddy-k/pacificdb-community
   gh variable set GPG_FINGERPRINT --env maven-central \
     --repo hitesh-reddy-k/pacificdb-community --body YOUR_FULL_FINGERPRINT
   ```

   `gh secret set GPG_PASSPHRASE` prompts privately. To encode the Portal token
   without putting its values in command history or printing them, use:

   ```sh
   python3 -c 'import base64,getpass; u=getpass.getpass("Portal token username: "); p=getpass.getpass("Portal token password: "); print(base64.b64encode((u+":"+p).encode()).decode())' \
     | gh secret set CENTRAL_TOKEN --env maven-central \
         --repo hitesh-reddy-k/pacificdb-community
   rm -f "$signing_dir/key.asc"
   rmdir "$signing_dir"
   ```

7. Once the validated tag exists, run Actions → **Validate tested Java SDK on Central**
   with branch `v1.2` and release tag `v1.2.0-rc.2`:

   ```sh
   gh workflow run java-publish.yml \
     --repo hitesh-reddy-k/pacificdb-community --ref v1.2 \
     -f release_tag=v1.2.0-rc.2
   gh run list --repo hitesh-reddy-k/pacificdb-community \
     --workflow java-publish.yml --limit 3
   ```

8. This workflow signs the **tested** JAR/POM/source/Javadoc bytes, uploads a
   `USER_MANAGED` deployment and stops at **VALIDATED**. It does **not** publish.
   Open the deployment in Central Publisher Portal, inspect its coordinates,
   version and validation results, then click **Publish**. Wait for **PUBLISHED**.
9. Verify public dependency resolution in a fresh Maven consumer/repository:

   ```sh
   mvn -U -Dmaven.repo.local=/tmp/pacificdb-central-rc-consumer \
     org.apache.maven.plugins:maven-dependency-plugin:3.8.1:get \
     -Dartifact=io.pacificdb:pacificdb-client:1.2.0-rc.2
   ```

   Then execute the Java example in SDK_GUIDE.md against the candidate engine.
   Central propagation may take time. `VALIDATED` alone is not externally available.
   Published versions cannot be overwritten.

Official references: [Portal token](https://central.sonatype.org/publish/generate-portal-token/),
[GPG signing](https://central.sonatype.org/publish/requirements/gpg/),
[namespace verification](https://central.sonatype.org/register/namespace/),
[USER_MANAGED deployment API](https://central.sonatype.org/publish/publish-portal-api/).

## 3. What to send back

Send only the successful Actions run URL(s), PyPI package URL, Central deployment
ID/status and public Maven artifact URL. If a step fails, send the sanitized error
and run URL. **Do not send passwords, Portal tokens, private keys, or secret values.**
I can then independently download/install the public bytes and verify examples.

GitHub release, npm and container publication are separate channels. This guide
covers the owner-only PyPI/Maven setup you offered to handle; it does not claim
that any destination is already published. Benchmarks use downloaded native/SDK
release artifacts after actual publication and retain their source/binary hashes.
