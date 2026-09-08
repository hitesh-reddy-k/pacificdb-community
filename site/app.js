const repository = 'hitesh-reddy-k/pacificdb-community';
const releasePage = `https://github.com/${repository}/releases`;
let assets = [];

function updateDownloads() {
  for (const link of document.querySelectorAll('[data-platform]')) {
    const asset = assets.find(item => item.name.endsWith(`-${link.dataset.platform}`));
    // Only link to assets actually present in a public release for this repository.
    const prefix = `https://github.com/${repository}/releases/download/`;
    if (asset && asset.browser_download_url.startsWith(prefix)) {
      link.href = asset.browser_download_url;
      link.textContent = `Download ${link.dataset.platform.split('.').pop()} ↓`;
    } else {
      link.href = releasePage;
      link.textContent = 'Check release availability ↗';
    }
  }
}

document.querySelector('#mac-arch').addEventListener('change', event => {
  document.querySelector('#mac-download').dataset.platform = event.target.value;
  updateDownloads();
});

for (const button of document.querySelectorAll('[data-copy]')) {
  button.addEventListener('click', async () => {
    const text = document.getElementById(button.dataset.copy).textContent;
    try {
      await navigator.clipboard.writeText(text);
      button.textContent = 'Copied!';
      document.querySelector('#copy-status').textContent = 'Copied to clipboard.';
      setTimeout(() => { button.textContent = 'Copy'; }, 2000);
    } catch {
      document.querySelector('#copy-status').textContent = 'Clipboard unavailable. Select and copy the command manually.';
      const range = document.createRange();
      range.selectNodeContents(document.getElementById(button.dataset.copy));
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
    }
  });
}

async function loadRelease() {
  const status = document.querySelector('#release-status');
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    // The releases list includes prereleases; /latest excludes Community beta builds.
    const response = await fetch(`https://api.github.com/repos/${repository}/releases?per_page=10`, {
      signal: controller.signal, headers: { Accept: 'application/vnd.github+json' }
    });
    if (!response.ok) throw new Error('Release lookup unavailable');
    const releases = await response.json();
    const release = releases.find(item => !item.draft && item.assets?.some(asset => /\.(deb|exe|pkg)$/.test(asset.name)));
    if (!release) throw new Error('Installers have not been published yet');
    assets = release.assets;
    status.textContent = `${release.tag_name} · Downloads available for published platforms`;
  } catch {
    status.textContent = 'Check GitHub Releases for available installers.';
  } finally {
    clearTimeout(timeout);
    updateDownloads();
  }
}
loadRelease();
