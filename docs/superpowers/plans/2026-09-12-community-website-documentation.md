# PacificDB Community Website Documentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a searchable, responsive beta.9 documentation page that lets users install PacificDB, use the shell, connect supported SDKs, operate data features, and troubleshoot common failures without leaving the website.

**Architecture:** Keep GitHub Pages static and dependency-free. A semantic HTML page owns the documentation content, a focused stylesheet owns the documentation layout, and a small progressive-enhancement script owns local search, navigation state, and clipboard behavior; a Node standard-library test validates the public documentation contract.

**Tech Stack:** HTML5, CSS, browser JavaScript, Node.js 18+ standard library, GitHub Pages.

**Spec:** `docs/superpowers/specs/2026-09-12-community-website-documentation-design.md`

## Global Constraints

- Describe only behavior certified for PacificDB Community `0.1.0-beta.9`.
- State that the public npm `beta` tag currently installs `0.1.0-beta.7`.
- State that Windows signing and macOS notarization remain pending.
- Use the existing supplied PacificDB logo and established site styles.
- Do not add dependencies, frameworks, analytics, server endpoints, or a documentation build system.
- Keep the page readable and navigable when JavaScript is unavailable.
- Never include passwords, session tokens, complete API keys, or internal engine fields.

---

### Task 1: Lock the public documentation contract

**Files:**
- Create: `scripts/test-site-docs.mjs`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- Consumes: static files under `site/`.
- Produces: `node scripts/test-site-docs.mjs`, exiting zero only when the documentation contract is complete and internally consistent.

- [ ] **Step 1: Write the failing static-site test**

Create a Node standard-library script that reads `site/index.html`,
`site/docs.html`, `site/docs.css`, and `site/docs.js`. It validates real local
file/fragment navigation and the exported search matcher. The stable public
section contract is:

```js
const requiredIds = [
  'install', 'quickstart', 'authentication', 'projects', 'databases',
  'documents', 'shell-reference', 'nodejs', 'python', 'java', 'backups',
  'api-keys', 'media', 'vectors', 'configuration', 'security',
  'troubleshooting', 'beta-status'
];

for (const id of requiredIds) {
  assert.match(docs, new RegExp(`id=["']${id}["']`));
  assert.match(docs, new RegExp(`href=["']#${id}["']`));
}
assert.match(docs, /pacificdb-logo\.png/);
assert.deepEqual(
  filterDocumentationItems(['Install PacificDB', 'Vector search'], 'VECTOR'),
  [false, true]
);
```

Parse every local `href`, `src`, and fragment reference and assert that its
target file or element exists. Assert that the CSS includes a narrow-screen
media query and that the script includes clipboard support. Review version
copy, beta disclosures, and product-boundary wording editorially rather than
locking human prose into change-detector assertions.

- [ ] **Step 2: Run the test and verify RED**

Run: `node scripts/test-site-docs.mjs`

Expected: FAIL because `site/docs.html`, `site/docs.css`, and `site/docs.js` do
not exist.

- [ ] **Step 3: Add the check to the retained entry point**

Add this command near the existing JavaScript checks in
`scripts/test-community.sh`:

```bash
node scripts/test-site-docs.mjs
```

- [ ] **Step 4: Commit the failing contract**

```bash
git add scripts/test-site-docs.mjs scripts/test-community.sh
git commit -m "test(site): define documentation contract"
```

### Task 2: Build the complete documentation page

**Files:**
- Create: `site/docs.html`

**Interfaces:**
- Consumes: `site/pacificdb-logo.png`, beta.9 release URLs, certified command forms from the repository READMEs and shell help.
- Produces: stable documentation anchors listed by Task 1 and semantic searchable sections marked with `data-doc-section`.

- [ ] **Step 1: Add the semantic page shell**

Create a page with a shared brand header, `Documentation` active navigation,
beta.9 badge, search input, grouped sidebar links, `<main id="main">`, and an
empty search-state element. Each sidebar target must be a real section ID.

- [ ] **Step 2: Add installation and quickstart content**

Document the Linux, Windows, and macOS native installers, npm beta.7, Docker,
source builds, automatic local-engine startup, login/bootstrap behavior, and
this verified first workflow:

```text
create project demo
list projects
create database hello
use hello
create collection ideas
insert ideas {"id":"first","title":"Hello PacificDB"}
find ideas {"id":"first"}
```

- [ ] **Step 3: Add the complete shell reference**

Use tables grouped by authentication, projects, databases/queries, backups,
API keys, media, vectors, and system commands. Copy command spellings exactly
from `cli/src/shell.js::SHELL_HELP` and `engine/src/community_shell.cpp`.

- [ ] **Step 4: Add SDK examples**

Document Node.js, Python, and Java install coordinates and minimal create,
insert, and find examples copied from the respective repository SDK READMEs.
Explain that applications connect to a running engine over JSON/TCP.

- [ ] **Step 5: Add operations and troubleshooting**

Cover backups/restores, API-key roles, sequential media, supported vector
metrics, host/port and local file paths, TLS/authentication advice, port
conflicts, unavailable engines, installer prompts, npm/native differences,
beta limitations, GitHub issues, and the certification report.

- [ ] **Step 6: Run the contract test and confirm the remaining failure**

Run: `node scripts/test-site-docs.mjs`

Expected: FAIL only because CSS and JavaScript assets are still absent or do
not yet contain their required behavior.

- [ ] **Step 7: Commit the content slice**

```bash
git add site/docs.html
git commit -m "docs(site): add Community documentation"
```

### Task 3: Add responsive presentation and progressive behavior

**Files:**
- Create: `site/docs.css`
- Create: `site/docs.js`

**Interfaces:**
- Consumes: `[data-doc-search]`, `[data-doc-link]`, `[data-doc-section]`, `[data-copy]`, and section IDs from `site/docs.html`.
- Produces: local `filterDocumentation(query)`, clipboard controls, active navigation, and mobile navigation without network calls.

- [ ] **Step 1: Implement the documentation layout**

Use the existing color variables and typography. Add a sticky desktop sidebar,
constrained article width, section dividers, callouts, tables, code blocks,
focus styles, and `overflow-x:auto` for tables and examples. Add a
`@media(max-width: 760px)` layout that places navigation above content and
supports the native details/summary mobile navigation.

- [ ] **Step 2: Implement local search**

Normalize the input query, compare it with each section's text, hide unmatched
sections and links with the standard `hidden` attribute, show the empty state
when no section matches, and restore all content for an empty query. Export
`filterDocumentation` for the test environment only when `module.exports`
exists; browser use remains dependency-free.

- [ ] **Step 3: Implement copy and navigation behavior**

Use `navigator.clipboard.writeText` when available. On failure, select the
source element and update the `role="status"` element. Use `IntersectionObserver`
when available to mark the current sidebar link with `aria-current="location"`;
ordinary fragment links remain the fallback.

- [ ] **Step 4: Run focused checks and verify GREEN**

Run:

```bash
node scripts/test-site-docs.mjs
node --check site/docs.js
git diff --check
```

Expected: all commands exit zero.

- [ ] **Step 5: Commit the interactive presentation**

```bash
git add site/docs.css site/docs.js
git commit -m "feat(site): add searchable documentation navigation"
```

### Task 4: Connect the landing page to documentation

**Files:**
- Modify: `site/index.html`
- Modify: `scripts/test-site-docs.mjs`

**Interfaces:**
- Consumes: `site/docs.html` and its stable section IDs.
- Produces: visible links from the primary header, quickstart, and footer to the documentation page.

- [ ] **Step 1: Add and run the failing landing-page assertion**

Add this assertion to `scripts/test-site-docs.mjs`:

```js
assert.match(index, /href=["']docs\.html["'][^>]*>Documentation</);
```

Run: `node scripts/test-site-docs.mjs`

Expected: FAIL because the landing-page header does not link to documentation.

- [ ] **Step 2: Update landing-page navigation**

Add `<a href="docs.html">Documentation</a>` to the primary navigation. Change
the current external README quickstart link to `docs.html#quickstart` and add a
footer documentation link.

- [ ] **Step 3: Run focused checks**

Run:

```bash
node scripts/test-site-docs.mjs
node --check site/app.js
node --check site/docs.js
git diff --check
```

Expected: all commands exit zero.

- [ ] **Step 4: Commit landing-page integration**

```bash
git add site/index.html scripts/test-site-docs.mjs
git commit -m "docs(site): link product page to documentation"
```

### Task 5: Verify and publish the documentation

**Files:**
- Modify: `docs/COMMUNITY_P0_CERTIFICATION.md` only if the retained count changes.

**Interfaces:**
- Consumes: all prior tasks and the existing GitHub Pages workflow.
- Produces: a clean `main` commit and deployed public documentation URL.

- [ ] **Step 1: Run the retained verification**

Run:

```bash
node scripts/test-site-docs.mjs
node --check site/app.js
node --check site/docs.js
git diff --check
scripts/test-community.sh build
```

Expected: all retained suites pass. If the site check adds one reported unit,
update the certification table and total from the fresh output.

- [ ] **Step 2: Review the final boundary**

Run:

```bash
! rg -i 'enterprise|billing|autoscal|saml|oidc|kms|hsm' site
git status --short
git log --oneline origin/main..HEAD
```

Expected: no forbidden product claims and only planned files/commits.

- [ ] **Step 3: Push the completed work to main**

```bash
git push origin HEAD:main
```

- [ ] **Step 4: Verify GitHub Pages and public links**

Wait for the Pages workflow for the pushed commit. Require HTTP 200 for
`index.html`, `docs.html`, `docs.css`, `docs.js`, and `pacificdb-logo.png`.
Require the deployed landing page to contain `href="docs.html"` and the deployed
documentation to contain beta.9, the complete section set, and the npm beta.7
disclosure.

- [ ] **Step 5: Clean generated files and report evidence**

Remove generated `build/` and `node_modules/` directories, fetch `origin/main`,
and require `HEAD == origin/main` with an empty `git status --short` result.
Report the documentation URL, commit, Pages workflow, test results, and the
remaining unsigned-installer/npm publication limitations.
