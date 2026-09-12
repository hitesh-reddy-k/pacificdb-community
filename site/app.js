const releaseBase = 'https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v0.1.0-beta.9';

function updateDownloads() {
  for (const link of document.querySelectorAll('[data-platform]')) {
    link.href = `${releaseBase}/pacificdb-community-0.1.0-beta.9-${link.dataset.platform}`;
    link.textContent = `Download ${link.dataset.platform.split('.').pop()} ↓`;
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

updateDownloads();
