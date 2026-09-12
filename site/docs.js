function filterDocumentationItems(items, query) {
  const normalizedQuery = String(query).trim().toLocaleLowerCase();
  if (!normalizedQuery) return items.map(() => true);
  return items.map(item => String(item).toLocaleLowerCase().includes(normalizedQuery));
}

function initializeDocumentation() {
  const search = document.querySelector('[data-doc-search]');
  const sections = [...document.querySelectorAll('[data-doc-section]')];
  const links = [...document.querySelectorAll('[data-doc-link]')];
  const groups = [...document.querySelectorAll('[data-doc-nav-group]')];
  const empty = document.querySelector('[data-doc-empty]');
  const status = document.querySelector('#docs-status');
  const navigationShell = document.querySelector('.docs-nav-shell');
  const narrowScreen = window.matchMedia('(max-width: 760px)');

  function fitNavigation(event) {
    if (navigationShell) navigationShell.open = !event.matches;
  }

  fitNavigation(narrowScreen);
  narrowScreen.addEventListener?.('change', fitNavigation);

  function setCurrentLink(sectionId) {
    for (const link of links) {
      if (link.getAttribute('href') === `#${sectionId}`) {
        link.setAttribute('aria-current', 'location');
      } else {
        link.removeAttribute('aria-current');
      }
    }
  }

  function applySearch() {
    const matches = filterDocumentationItems(
      sections.map(section => section.textContent), search?.value ?? ''
    );
    sections.forEach((section, index) => { section.hidden = !matches[index]; });
    links.forEach(link => {
      const section = document.querySelector(link.getAttribute('href'));
      link.hidden = !section || section.hidden;
    });
    groups.forEach(group => {
      group.hidden = !group.querySelector('[data-doc-link]:not([hidden])');
    });
    if (empty) empty.hidden = matches.some(Boolean);
  }

  search?.addEventListener('input', applySearch);

  for (const button of document.querySelectorAll('[data-copy]')) {
    button.type = 'button';
    button.addEventListener('click', async () => {
      const source = document.getElementById(button.dataset.copy);
      if (!source) return;
      const value = source.textContent.trim();
      try {
        if (!navigator.clipboard?.writeText) throw new Error('clipboard unavailable');
        await navigator.clipboard.writeText(value);
        button.textContent = 'Copied!';
        if (status) status.textContent = 'Copied to clipboard.';
      } catch {
        const selection = window.getSelection();
        const range = document.createRange();
        range.selectNodeContents(source);
        selection.removeAllRanges();
        selection.addRange(range);
        if (status) status.textContent = 'Clipboard unavailable. The example is selected for manual copying.';
      }
      window.setTimeout(() => { button.textContent = 'Copy'; }, 1800);
    });
  }

  for (const link of links) {
    link.addEventListener('click', () => setCurrentLink(link.hash.slice(1)));
  }

  if ('IntersectionObserver' in window) {
    const observer = new IntersectionObserver(entries => {
      const visible = entries
        .filter(entry => entry.isIntersecting && !entry.target.hidden)
        .sort((left, right) => left.boundingClientRect.top - right.boundingClientRect.top);
      if (visible[0]) setCurrentLink(visible[0].target.id);
    }, { rootMargin: '-12% 0px -72% 0px' });
    sections.forEach(section => observer.observe(section));
  }

  const initialSection = window.location.hash.slice(1) || sections[0]?.id;
  if (initialSection) setCurrentLink(initialSection);
  applySearch();
}

if (typeof document !== 'undefined') initializeDocumentation();

if (typeof module !== 'undefined') module.exports = { filterDocumentationItems };
