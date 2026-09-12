# PacificDB Community Website Documentation Design

**Date:** 2026-09-12

## Goal

Add a documentation experience to the public PacificDB Community website so a
new user can install beta.9, start the local database, learn the shell, connect
an application, and resolve common problems without leaving the website.

## Scope

The website gains one dedicated static documentation page at `site/docs.html`.
The existing landing page links to it from the primary navigation and its
quickstart section. The page covers:

- Linux, Windows, macOS, npm, Docker, and source installation paths
- automatic local-engine startup, connection options, login, and local files
- the first project, database, collection, and document workflow
- the complete certified native/npm shell command catalog
- Node.js, Python, and Java connection examples
- manual backup and restore, API keys, media transfer, and vector search
- configuration, security guidance, troubleshooting, beta status, and support

The documentation describes only beta.9 behavior certified by the repository.
The npm section clearly states that the public npm `beta` tag remains beta.7.
Unsigned Windows and macOS installer limitations remain visible.

## Approach

Keep the site framework-free. `docs.html` contains semantic documentation
sections and code examples. `docs.css` extends the current visual language with
a sticky sidebar, readable article column, command tables, callouts, and mobile
layout. `docs.js` supplies small progressive enhancements: navigation search,
active-section highlighting, mobile navigation, and copy buttons. The page
remains readable and navigable if JavaScript is unavailable.

This is preferable to placing all reference material on the landing page,
which would make the product page difficult to scan. It also avoids adopting a
documentation generator and build pipeline for the current beta-sized corpus.

## Information Architecture

The documentation page uses stable fragment identifiers and groups content as
follows:

1. **Start here** — overview, beta notice, requirements, install, start, and
   first document.
2. **Use the shell** — authentication, projects, databases, collections,
   documents, aggregation, explain, and local system commands.
3. **Application SDKs** — Node.js, Python, and Java installation and working
   connection examples.
4. **Data operations** — backups/restores, API keys, media, and vectors.
5. **Operate PacificDB** — connection/configuration, local paths, security,
   troubleshooting, release limits, and support links.

The left navigation mirrors these groups. Search filters navigation entries and
matching documentation sections using visible text; it does not call a server
or collect search data.

## Content Sources and Accuracy

`README.md`, `cli/README.md`, SDK READMEs, the shell help constants, and
`docs/COMMUNITY_P0_CERTIFICATION.md` are the sources of truth. Command spelling
and examples must match the native and npm shell parsers. Examples must use
public fields and must not show passwords, session tokens, or complete API keys.

The page distinguishes the native installer, which includes `db_engine`, from
the npm CLI, which is a client and can auto-start only when a compatible native
engine is already installed. Port-conflict troubleshooting explains the
beta.9 protocol check and the `--port` option.

## User Interface

The documentation header uses the supplied PacificDB logo and the same brand,
colors, typography, and GitHub link as the landing page. It adds a visible
`Documentation` navigation item and a beta.9 version badge.

On desktop, a sticky sidebar provides search and grouped links while the main
article uses a constrained reading width. On small screens, navigation appears
above the article and can be expanded or collapsed. Tables scroll horizontally
when necessary. Every code sample has an accessible copy button and a live
status message.

## Error and Empty States

- An empty search restores the complete navigation and document.
- A search with no match displays a plain `No documentation found` message.
- Clipboard failure selects the sample and tells the user to copy it manually.
- All behavior remains usable through ordinary links when JavaScript fails.

## Files

- Create `site/docs.html`
- Create `site/docs.css`
- Create `site/docs.js`
- Modify `site/index.html`
- Add a lightweight standard-library site documentation check under `scripts/`
- Run that check from the retained Community test entry point if it remains
  fast and independent

No framework, package dependency, server endpoint, analytics service, or
documentation build system is added.

## Verification

Automated checks verify required sections, stable fragment targets, internal
navigation links, beta.9 installer references, npm beta.7 disclosure, logo
usage, and absence of unsupported Enterprise/Cloud terms. JavaScript syntax and
repository diff checks must pass.

The completed GitHub Pages deployment is then checked for:

- a working Documentation link from the landing page
- successful loading of `docs.html`, `docs.css`, `docs.js`, and the logo
- visible install, quickstart, shell, SDK, operations, and troubleshooting
  content
- valid beta.9 release links
- usable desktop and narrow-screen layouts

## Acceptance Criteria

1. A visitor can reach documentation directly from the website header.
2. A new user can install and create/read a document using only the website.
3. Every certified shell command category is documented and searchable.
4. Node.js, Python, and Java users have copyable connection examples.
5. Backup, API-key, media, vector, security, and troubleshooting guidance is
   present and accurate for beta.9.
6. Beta limitations and the older public npm version are stated clearly.
7. The page is keyboard-accessible, responsive, and useful without JavaScript.
8. Static checks and the existing retained test suite pass before publication.
