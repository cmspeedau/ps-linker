/* One place that talks to the share's JSON API. Absolute paths: the shell
 * is served under /gui/, and the API is not (it must stay reachable for a
 * stable contract the PSP/PS3 world never touches either way). */

const CATALOG_URL = '/api/catalog';
const RAW_UPLOAD_URL = '/api/upload/file';

export async function fetchCatalog() {
  const r = await fetch(CATALOG_URL, { cache: 'no-cache' });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  const data = await r.json();
  return Array.isArray(data.items) ? data.items : [];
}

async function jsend(method, url, body) {
  const r = await fetch(url, {
    method,
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  let data = null;
  try { data = await r.json(); } catch (e) { /* not JSON */ }
  if (!r.ok) throw new Error((data && data.error) || 'HTTP ' + r.status);
  return data;
}

/** Every manage mutation answers with the full merged items array. */
export const deleteItem = (url) =>
  jsend('DELETE', '/api/item?url=' + encodeURIComponent(url));
export const renameItem = (url, name) =>
  jsend('POST', '/api/item/rename', { url, name });
export const moveItem = (url, category) =>
  jsend('POST', '/api/item/move', { url, category });
export const setItemMeta = (url, patch) =>
  jsend('PUT', '/api/item/meta', Object.assign({ url }, patch));

/* ---- cover art ----
 * The server stores covers as the hidden '<stem>.icon0.<ext>' sidecar
 * cover_art.py uses, so an upload and an extraction are the same file and
 * tiles/icons need no special handling: every response carries the updated
 * items. */
export const extractCover = (url) =>
  jsend('POST', '/api/item/cover/extract', { url });
export const clearCover = (url) =>
  jsend('POST', '/api/item/cover/clear', { url });

/** Raw-body image upload like uploadFile, but small and capped at 8 MiB,
 *  so a plain fetch is enough — no progress plumbing. */
export async function uploadCover(url, file) {
  const r = await fetch('/api/item/cover?url=' + encodeURIComponent(url), {
    method: 'POST',
    headers: { 'Content-Type': file.type || 'application/octet-stream' },
    body: file,
  });
  let data = null;
  try { data = await r.json(); } catch (e) { /* not JSON */ }
  if (!r.ok) throw new Error((data && data.error) || 'HTTP ' + r.status);
  return data;
}

/* ---- Store (queue the server-side fetcher works through) ---- */
export const storeStatus = () => jsend('GET', '/api/store/status');
export const storeQueue = () => jsend('GET', '/api/store/queue');
export const storeAdd = (item) => jsend('POST', '/api/store/queue', item);
export const storeRemove = (url) =>
  jsend('DELETE', '/api/store/queue?url=' + encodeURIComponent(url));
export const storeRun = () => jsend('POST', '/api/store/run');

/**
 * Raw-body upload: the file streams as the request body to the server's
 * .part file (a 9 GB ISO never enters the server's memory, and multipart
 * stays only what it is, for curl). onProgress(loaded, total). `dir` is an
 * optional existing library folder — how a license .rap lands beside its
 * game.
 */
export function uploadFile(file, platform, onProgress, dir) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    let url = RAW_UPLOAD_URL + '?name=' + encodeURIComponent(file.name) +
              '&platform=' + encodeURIComponent(platform);
    if (dir) url += '&dir=' + encodeURIComponent('/' + dir);
    xhr.open('POST', url);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) onProgress(e.loaded, e.total);
    };
    xhr.onload = () => {
      let data = null;
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
      if (xhr.status === 201 && data && data.items) resolve(data);
      else reject(new Error((data && data.error) || 'HTTP ' + xhr.status));
    };
    xhr.onerror = () => reject(new Error('network error'));
    xhr.send(file);
  });
}
