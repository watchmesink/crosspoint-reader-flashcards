'use strict';
// CrossPoint Cards — local-first client.
//
// The browser runs the SAME SM-2++ engine the firmware and server run
// (window.CPEngine from /engine.js), over decks/progress cached in IndexedDB.
// Studying, stats and file edits all happen locally, so the app works fully
// offline. When online it reconciles with the server using the very endpoints
// the device already uses: /api/sync/<deck>/progress (merge) for progress and
// /api/sync/<deck>/manifest + file PUT/DELETE for deck files. Reconcile is
// merge-based and idempotent, so the browser, the device and the server all
// converge without clobbering each other.

const E = window.CPEngine;
const $ = (id) => document.getElementById(id);
const DECKS = [
  { id: 'german', label: 'German' },
  { id: 'ukrainian', label: 'Ukrainian' },
  { id: 'english', label: 'English' },
];
const FLAGS = { german: '\u{1F1E9}\u{1F1EA}', ukrainian: '\u{1F1FA}\u{1F1E6}', english: '\u{1F1EC}\u{1F1E7}' };
const flame = (n) => (n > 0 ? '\u{1F525}' + n : '');
// server sends device-parity memory strings ("Memory: Young | EF 2.50 | Ivl 6")
const memoryLabel = (s) => (s || '').replace(/^Memory:\s*/, '').replace(/\s*\|\s*/g, ' · ');

// Streak days follow the browser's local calendar, not UTC: studying just after
// midnight must count as the new local day or daily streaks show phantom gaps.
function browserUnixDay(nowMs = Date.now()) {
  const d = new Date(nowMs);
  return Math.floor(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / 86400000);
}
// The stored streak only updates on rating; a lapsed streak shows 0, not the
// stale last value.
function effectiveStreakDays(state) {
  if (!state || state.lastStudyUnixDay < 0) return 0;
  return browserUnixDay() - state.lastStudyUnixDay > 1 ? 0 : state.streakDays;
}

// ---- token ----------------------------------------------------------------
let token = localStorage.getItem('cp_token') || '';
const urlToken = new URLSearchParams(location.search).get('token');
if (urlToken) {
  token = urlToken;
  localStorage.setItem('cp_token', token);
  history.replaceState(null, '', location.pathname + location.hash);
}
const getToken = () => token;
function setToken(t) {
  token = t || '';
  if (token) localStorage.setItem('cp_token', token);
  else localStorage.removeItem('cp_token');
}

// ---- tiny helpers ---------------------------------------------------------
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast._t);
  toast._t = setTimeout(() => t.classList.remove('show'), 2200);
}

function debounce(fn, ms) {
  let h;
  return (...a) => { clearTimeout(h); h = setTimeout(() => fn(...a), ms); };
}

function timeAgo(iso) {
  if (!iso) return 'never';
  const s = (Date.now() - new Date(iso).getTime()) / 1000;
  if (s < 0) return 'just now';
  if (s < 90) return 'just now';
  if (s < 3600) return Math.round(s / 60) + ' min ago';
  if (s < 86400 * 2) return Math.round(s / 3600) + ' h ago';
  return Math.round(s / 86400) + ' days ago';
}

async function sha256Hex(str) {
  const data = new TextEncoder().encode(str);
  const buf = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// ---- IndexedDB ------------------------------------------------------------
const DB_NAME = 'cp-cards';
const DB_VERSION = 1;
let _db = null;

function idb() {
  if (_db) return Promise.resolve(_db);
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains('files')) {
        const files = db.createObjectStore('files', { keyPath: 'id' }); // id = `${deck}/${name}`
        files.createIndex('deck', 'deck', { unique: false });
      }
      if (!db.objectStoreNames.contains('state')) db.createObjectStore('state', { keyPath: 'deck' });
      if (!db.objectStoreNames.contains('meta')) db.createObjectStore('meta', { keyPath: 'deck' });
      if (!db.objectStoreNames.contains('kv')) db.createObjectStore('kv', { keyPath: 'k' });
    };
    req.onsuccess = () => { _db = req.result; resolve(_db); };
    req.onerror = () => reject(req.error);
  });
}

function tx(store, mode, fn) {
  return idb().then(
    (db) =>
      new Promise((resolve, reject) => {
        const t = db.transaction(store, mode);
        const s = t.objectStore(store);
        let out;
        Promise.resolve(fn(s)).then((v) => { out = v; });
        t.oncomplete = () => resolve(out);
        t.onerror = () => reject(t.error);
        t.onabort = () => reject(t.error);
      }),
  );
}
const reqP = (r) => new Promise((res, rej) => { r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });

const idbGet = (store, key) => tx(store, 'readonly', (s) => reqP(s.get(key)));
const idbPut = (store, val) => tx(store, 'readwrite', (s) => reqP(s.put(val)));
const idbDel = (store, key) => tx(store, 'readwrite', (s) => reqP(s.delete(key)));
const idbAll = (store) => tx(store, 'readonly', (s) => reqP(s.getAll()));

async function filesForDeck(deck) {
  const all = await idbAll('files');
  return all.filter((f) => f.deck === deck).sort((a, b) => E.naturalCompare(a.name, b.name));
}
const kvGet = (k) => idbGet('kv', k).then((r) => (r ? r.v : undefined));
const kvPut = (k, v) => idbPut('kv', { k, v });

// Global app settings (batch size), cached in IndexedDB so study honors the
// configured size even offline. Server is source of truth; a local change is
// marked dirty and pushed on the next reconcile.
const DEFAULT_SETTINGS = { batchSize: E.BATCH_SIZE, minBatchSize: E.MIN_BATCH_SIZE, maxBatchSize: E.BATCH_SIZE };
async function getSettings() {
  const s = await kvGet('appSettings');
  return { ...DEFAULT_SETTINGS, ...(s || {}) };
}
const getBatchSize = async () => E.normalizeBatchSize((await getSettings()).batchSize);

// ---- local data layer (mirrors the server's deck endpoints, but offline) --
async function loadLocalDeck(deck) {
  const files = (await filesForDeck(deck)).map((f) => ({ name: f.name, content: f.content }));
  const prev = await idbGet('state', deck);
  const { cards, state, changed } = E.loadDeck(files, prev || null, await getBatchSize());
  state.deck = deck;
  if (changed || !prev) await idbPut('state', state);
  return { cards, state };
}

async function localDeckList() {
  const out = [];
  for (const d of DECKS) {
    const { cards, state } = await loadLocalDeck(d.id);
    const meta = (await idbGet('meta', d.id)) || {};
    out.push({
      deck: d.id,
      label: d.label,
      total: cards.length,
      memorized: E.countMemorized(state, cards),
      streakDays: effectiveStreakDays(state),
      reviewStep: state.reviewStep,
      dueNow: E.countDue(state),
      files: (await filesForDeck(d.id)).length,
      lastSyncAt: meta.lastSyncAt || null,
    });
  }
  return out;
}

function viewFrom(deck, label, cards, state, meta) {
  return {
    deck,
    label,
    total: cards.length,
    memorized: E.countMemorized(state, cards),
    streakDays: effectiveStreakDays(state),
    reviewStep: state.reviewStep,
    batchSize: state.batch.length,
    batchProcessed: state.batch.filter((b) => b.processed).length,
    dueNow: E.countDue(state),
    current: E.projectCurrent(cards, state),
    statusMessage: '',
    lastSyncAt: (meta && meta.lastSyncAt) || null,
  };
}

async function localDeckView(deck) {
  const def = DECKS.find((d) => d.id === deck);
  const { cards, state } = await loadLocalDeck(deck);
  const meta = (await idbGet('meta', deck)) || {};
  return viewFrom(deck, def.label, cards, state, meta);
}

async function localRate(deck, key, rating) {
  const def = DECKS.find((d) => d.id === deck);
  const { cards, state } = await loadLocalDeck(deck);
  if (!E.rateCard(state, cards, key, E.RATING[rating.toUpperCase()], Date.now(), await getBatchSize(), browserUnixDay())) throw new Error('card not found');
  state.deck = deck;
  await idbPut('state', state);
  const meta = (await idbGet('meta', deck)) || {};
  scheduleReconcile();
  return viewFrom(deck, def.label, cards, state, meta);
}

async function localFilePut(deck, name, content) {
  await idbPut('files', { id: `${deck}/${name}`, deck, name, content, dirty: true });
  // clear any local tombstone so a re-created file isn't treated as deleted
  const meta = (await idbGet('meta', deck)) || { deck };
  if (meta.tombstones && name in meta.tombstones) { delete meta.tombstones[name]; await idbPut('meta', { ...meta, deck }); }
  await loadLocalDeck(deck); // reconcile records/batch with the new card set
  scheduleReconcile();
}

async function localFileDelete(deck, name) {
  await idbDel('files', `${deck}/${name}`);
  const meta = (await idbGet('meta', deck)) || { deck };
  meta.tombstones = { ...(meta.tombstones || {}), [name]: new Date().toISOString() };
  await idbPut('meta', { ...meta, deck });
  await loadLocalDeck(deck);
  scheduleReconcile();
}

const localFileGet = (deck, name) => idbGet('files', `${deck}/${name}`).then((f) => (f ? f.content : ''));

async function anyLocalData() {
  const [files, states] = await Promise.all([idbAll('files'), idbAll('state')]);
  return files.length > 0 || states.length > 0;
}

// ---- network + reconcile --------------------------------------------------
const fileUrl = (deck, name) => `/api/decks/${deck}/files/${encodeURIComponent(name)}`;

async function apiFetch(path, opts = {}) {
  const headers = { ...(opts.headers || {}) };
  if (token) headers['Authorization'] = 'Bearer ' + token;
  const res = await fetch(path, { ...opts, headers });
  if (res.status === 401) { const e = new Error('unauthorized'); e.code = 401; throw e; }
  if (!res.ok) {
    let msg = res.statusText;
    try { msg = (await res.json()).error || msg; } catch (_) {}
    throw new Error(msg);
  }
  const ct = res.headers.get('content-type') || '';
  return ct.includes('json') ? res.json() : res.text();
}

async function reconcileProgress(deck) {
  const localBefore = (await idbGet('state', deck)) || E.newDeckState();
  const studied = localBefore.reviewStep > 0 || localBefore.records.some((r) => r.reviewCount > 0);
  const bin = studied ? E.toBase64(E.serializeProgressBin(localBefore)) : null;
  const res = await apiFetch(`/api/sync/${deck}/progress`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ bin }),
  });
  const merged = res && res.bin ? E.parseProgressBin(E.fromBase64(res.bin)) : null;
  if (!merged) return;
  // Merge the server result back into whatever local is NOW (the user may have
  // rated again while this request was in flight). mergeDeckStates keeps the
  // higher-reviewStep side, so newer local work is never lost; idempotent.
  const localNow = (await idbGet('state', deck)) || E.newDeckState();
  const adopted = E.mergeDeckStates(localNow, merged);
  adopted.deck = deck;
  await idbPut('state', adopted);
  await loadLocalDeck(deck); // re-derive batch/current against the cards
}

async function reconcileFiles(deck) {
  const manifest = await apiFetch(`/api/sync/${deck}/manifest`);
  const serverByName = new Map((manifest.files || []).map((f) => [f.name, f]));
  const serverTomb = new Set(manifest.tombstones || []);
  const localFiles = await filesForDeck(deck);
  const localByName = new Map(localFiles.map((f) => [f.name, f]));
  const meta = (await idbGet('meta', deck)) || { deck };
  const synced = meta.syncedFiles || {};
  const names = new Set([...serverByName.keys(), ...localByName.keys()]);
  const convergedSynced = {};

  for (const name of names) {
    const onServer = serverByName.has(name);
    const onLocal = localByName.has(name);
    const known = name in synced;
    try {
      if (onServer && onLocal) {
        const lf = localByName.get(name);
        const localSha = await sha256Hex(lf.content);
        const serverSha = serverByName.get(name).sha256;
        if (localSha === serverSha) {
          if (lf.dirty) await idbPut('files', { ...lf, dirty: false });
          convergedSynced[name] = serverSha;
        } else if (lf.dirty) {
          // edited/created offline -> push local
          await apiFetch(fileUrl(deck, name), {
            method: 'PUT', body: lf.content, headers: { 'Content-Type': 'text/plain; charset=utf-8' },
          });
          await idbPut('files', { ...lf, dirty: false });
          convergedSynced[name] = localSha;
        } else {
          // changed on the server (skill/device) -> pull
          const content = await apiFetch(fileUrl(deck, name));
          await idbPut('files', { id: `${deck}/${name}`, deck, name, content, dirty: false });
          convergedSynced[name] = serverSha;
        }
      } else if (onLocal && !onServer) {
        const lf = localByName.get(name);
        if (known && !lf.dirty) {
          await idbDel('files', lf.id); // existed at last sync, removed on server -> drop locally
        } else {
          await apiFetch(fileUrl(deck, name), {
            method: 'PUT', body: lf.content, headers: { 'Content-Type': 'text/plain; charset=utf-8' },
          });
          await idbPut('files', { ...lf, dirty: false });
          convergedSynced[name] = await sha256Hex(lf.content);
        }
      } else {
        // on server only
        if (known && !serverTomb.has(name)) {
          await apiFetch(fileUrl(deck, name), { method: 'DELETE' }); // deleted locally -> delete on server
        } else if (!serverTomb.has(name)) {
          const content = await apiFetch(fileUrl(deck, name)); // new on server -> download
          await idbPut('files', { id: `${deck}/${name}`, deck, name, content, dirty: false });
          convergedSynced[name] = serverByName.get(name).sha256;
        }
      }
    } catch (e) {
      if (e && e.code === 401) throw e;
      if (known) convergedSynced[name] = synced[name]; // leave as-was, retry next pass
    }
  }
  await idbPut('meta', { ...meta, deck, syncedFiles: convergedSynced });
}

// Pull the global settings (batch size) from the server, or push a pending local
// change first. Server is source of truth; runs once per reconcile pass.
async function reconcileSettings() {
  const local = await getSettings();
  const res = local.dirty
    ? await apiFetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ batchSize: local.batchSize }),
      })
    : await apiFetch('/api/settings');
  await kvPut('appSettings', { ...res, dirty: false });
}

let reconciling = false;
let reconcilePending = false;
async function reconcileAll(reason) {
  if (!navigator.onLine) { setStatus('offline'); return; }
  if (!getToken()) { setStatus('locked'); return; }
  if (reconciling) { reconcilePending = true; return; }
  reconciling = true;
  setStatus('syncing');
  try {
    await reconcileSettings();
    for (const d of DECKS) {
      await reconcileFiles(d.id);
      await reconcileProgress(d.id);
      const meta = (await idbGet('meta', d.id)) || { deck: d.id };
      await idbPut('meta', { ...meta, deck: d.id, lastSyncAt: new Date().toISOString() });
    }
    await kvPut('lastSyncAt', new Date().toISOString());
    setStatus('online');
  } catch (e) {
    if (e && e.code === 401) { setStatus('locked'); showPin(); }
    else setStatus('offline');
  } finally {
    reconciling = false;
    if (reconcilePending) { reconcilePending = false; setTimeout(() => reconcileAll('pending'), 0); }
    else if (currentScreen !== 'study') route();
  }
}
const scheduleReconcile = debounce(() => reconcileAll('debounced'), 1200);

// ---- status pill ----------------------------------------------------------
async function setStatus(kind) {
  const el = $('status');
  if (!el) return;
  el.dataset.kind = kind;
  if (kind === 'syncing') { el.textContent = 'syncing…'; return; }
  if (kind === 'offline') { el.textContent = 'offline'; return; }
  if (kind === 'locked') { el.textContent = 'locked'; return; }
  const last = await kvGet('lastSyncAt');
  el.textContent = last ? 'synced ' + timeAgo(last) : 'synced';
}

// ---- routing --------------------------------------------------------------
let currentScreen = 'list';
window.addEventListener('hashchange', route);

function nav(hash) { location.hash = hash; }
function setHeader(title, { back = false, streak = '' } = {}) {
  $('title').textContent = title;
  $('backBtn').classList.toggle('hidden', !back);
  $('streak').textContent = streak;
}

async function route() {
  const parts = location.hash.replace(/^#\/?/, '').split('/').filter(Boolean);
  try {
    if (parts.length === 0) { currentScreen = 'list'; return await showDeckList(); }
    if (parts[0] === 'deck' && parts.length === 2) { currentScreen = 'home'; return await showDeckHome(parts[1]); }
    if (parts[0] === 'deck' && parts[2] === 'study') { currentScreen = 'study'; return await showStudy(parts[1]); }
    if (parts[0] === 'deck' && parts[2] === 'files') { currentScreen = 'files'; return await showFiles(parts[1]); }
    if (parts[0] === 'settings') { currentScreen = 'settings'; return await showSettings(); }
    nav('');
  } catch (e) {
    toast(e.message || String(e));
  }
}

// ---- screens --------------------------------------------------------------
async function showDeckList() {
  setHeader('CrossPoint Cards');
  const decks = await localDeckList();
  const main = $('main');
  main.classList.remove('no-scroll');
  main.innerHTML = '';
  $('streak').textContent = flame(Math.max(...decks.map((d) => d.streakDays), 0));

  const list = document.createElement('div');
  list.className = 'group';
  for (const d of decks) {
    const el = document.createElement('button');
    el.className = 'row';
    el.innerHTML = `
      <span class="avatar">${FLAGS[d.deck] || '\u{1F0CF}'}</span>
      <span class="row-main">
        <span class="row-title">${d.label}</span>
        <span class="row-sub">${d.memorized}/${d.total} known &middot; ${d.files} files &middot; synced ${timeAgo(d.lastSyncAt)}</span>
      </span>
      ${d.dueNow > 0 ? `<span class="due-pill">${d.dueNow} due</span>` : '<span class="chevron">&#8250;</span>'}`;
    el.onclick = () => nav('/deck/' + d.deck);
    list.appendChild(el);
  }
  main.appendChild(list);

  const settingsGroup = document.createElement('div');
  settingsGroup.className = 'group';
  const settingsBtn = document.createElement('button');
  settingsBtn.className = 'row';
  settingsBtn.innerHTML = `
    <span class="avatar">&#9881;&#65039;</span>
    <span class="row-main"><span class="row-title">Settings</span></span>
    <span class="chevron">&#8250;</span>`;
  settingsBtn.onclick = () => nav('/settings');
  settingsGroup.appendChild(settingsBtn);
  main.appendChild(settingsGroup);
}

async function showDeckHome(deck) {
  const v = await localDeckView(deck);
  setHeader(v.label, { back: true, streak: flame(v.streakDays) });
  const main = $('main');
  main.classList.remove('no-scroll');
  main.innerHTML = '';

  const tiles = document.createElement('div');
  tiles.className = 'tiles';
  tiles.innerHTML = `
    <div class="tile">
      <div class="tile-num">${v.memorized}<span class="tile-den"> / ${v.total}</span></div>
      <div class="tile-label">Known</div>
    </div>
    <div class="tile">
      <div class="tile-num">${v.streakDays ? '\u{1F525} ' + v.streakDays : '&mdash;'}</div>
      <div class="tile-label">Streak</div>
    </div>`;
  main.appendChild(tiles);

  const stats = document.createElement('div');
  stats.className = 'group';
  stats.innerHTML = `
    <div class="row"><span class="row-main"><span class="row-title">Batch</span></span><span class="row-sub">${v.batchProcessed} of ${v.batchSize} done</span></div>
    <div class="row"><span class="row-main"><span class="row-title">Reviews</span></span><span class="row-sub">${v.reviewStep}</span></div>
    <div class="row"><span class="row-main"><span class="row-title">Last sync</span></span><span class="row-sub">${timeAgo(v.lastSyncAt)}</span></div>`;
  main.appendChild(stats);

  if (v.total === 0) {
    const empty = document.createElement('p');
    empty.className = 'sub';
    empty.innerHTML = 'No cards yet &mdash; upload a .txt,<br>one term&#8677;translation per line.';
    main.appendChild(empty);
  } else {
    const learn = document.createElement('button');
    learn.className = 'btn primary';
    learn.textContent = v.dueNow > 0 ? `Study · ${v.dueNow} due` : 'Study';
    learn.onclick = () => nav('/deck/' + deck + '/study');
    main.appendChild(learn);
  }

  const files = document.createElement('button');
  files.className = 'btn';
  files.textContent = 'Files';
  files.onclick = () => nav('/deck/' + deck + '/files');
  main.appendChild(files);
}

async function showStudy(deck) {
  let v = await localDeckView(deck);
  if (!v.current) { nav('/deck/' + deck); return; }
  setHeader(v.label, { back: true });

  const main = $('main');
  main.classList.add('no-scroll');
  main.innerHTML = `
    <div class="study-top">
      <div class="progress"><div class="progress-fill" id="progressFill"></div></div>
      <div class="study-meta">
        <span id="counter"></span>
        <span id="memoryLine"></span>
      </div>
    </div>
    <div class="card-wrap">
      <div class="card" id="card">
        <div class="face prompt">
          <div class="word" id="promptText"></div>
          <div class="hint">Tap to reveal</div>
        </div>
        <div class="face answer">
          <div class="word" id="answerText"></div>
        </div>
      </div>
    </div>
    <div class="rate-row">
      <button class="btn hard" id="rateHard">Hard</button>
      <button class="btn good" id="rateGood">Good</button>
      <button class="btn easy" id="rateEasy">Easy</button>
    </div>`;

  const card = $('card');
  card.onclick = () => card.classList.toggle('flipped');

  function fitWord(el, text) {
    el.textContent = text;
    const len = text.length;
    el.style.fontSize = len > 120 ? '21px' : len > 60 ? '25px' : len > 30 ? '31px' : '40px';
  }

  function renderCurrent() {
    const c = v.current;
    $('counter').textContent = `Card ${c.batchPosition} of ${v.batchSize || 1}`;
    $('memoryLine').textContent = memoryLabel(c.memoryLine);
    $('progressFill').style.width = `${Math.round((v.batchProcessed / (v.batchSize || 1)) * 100)}%`;
    card.classList.remove('flipped');
    fitWord($('promptText'), c.prompt);
    fitWord($('answerText'), c.answer);
  }

  async function rate(rating) {
    for (const id of ['rateHard', 'rateGood', 'rateEasy']) $(id).disabled = true;
    try {
      v = await localRate(deck, v.current.key, rating);
      $('streak').textContent = flame(v.streakDays);
      if (!v.current) { nav('/deck/' + deck); return; }
      renderCurrent();
    } catch (e) {
      toast(e.message);
    } finally {
      for (const id of ['rateHard', 'rateGood', 'rateEasy']) $(id).disabled = false;
    }
  }

  $('rateHard').onclick = () => rate('hard');
  $('rateGood').onclick = () => rate('good');
  $('rateEasy').onclick = () => rate('easy');
  renderCurrent();
}

async function showFiles(deck) {
  setHeader('Files', { back: true });
  const files = await filesForDeck(deck);
  const main = $('main');
  main.classList.remove('no-scroll');
  main.innerHTML = '';

  const upload = document.createElement('div');
  upload.innerHTML = `
    <input type="file" id="filePick" accept=".txt" multiple class="hidden">
    <button class="btn" id="uploadBtn">Upload .txt files</button>
    <p class="sub" style="margin:14px 0 8px">or paste cards</p>
    <textarea id="pasteArea" placeholder="der Hund&#9;the dog&#10;die Katze&#9;the cat"></textarea>
    <div style="height:10px"></div>
    <button class="btn primary" id="pasteSave">Save cards</button>`;
  main.appendChild(upload);

  const list = document.createElement('div');
  list.className = 'group';
  main.appendChild(list);

  for (const f of files) {
    const row = document.createElement('div');
    row.className = 'row';
    row.style.cursor = 'default';
    row.innerHTML = `
      <span class="row-main file-name" style="cursor:pointer">
        <span class="row-title" style="font-size:15px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap">${f.name}</span>
        <span class="row-sub">${(f.content.length / 1024).toFixed(1)} KB${f.dirty ? ' · not synced yet' : ''}</span>
      </span>
      <button class="file-del">&#10005;</button>`;
    row.querySelector('.file-name').onclick = async () => {
      const content = await localFileGet(deck, f.name);
      let viewer = row.nextElementSibling;
      if (viewer && viewer.classList.contains('viewer')) { viewer.remove(); return; }
      viewer = document.createElement('pre');
      viewer.className = 'viewer';
      viewer.textContent = content;
      row.after(viewer);
    };
    row.querySelector('.file-del').onclick = async () => {
      if (!confirm('Delete ' + f.name + '? It will also be removed from the device on next sync.')) return;
      await localFileDelete(deck, f.name);
      toast('Deleted ' + f.name);
      showFiles(deck);
    };
    list.appendChild(row);
  }

  $('uploadBtn').onclick = () => $('filePick').click();
  $('filePick').onchange = async (e) => {
    for (const file of e.target.files) {
      const content = await file.text();
      await localFilePut(deck, file.name, content);
    }
    toast('Saved ' + e.target.files.length + ' file(s)');
    showFiles(deck);
  };
  $('pasteSave').onclick = async () => {
    const text = $('pasteArea').value.trim();
    if (!text) return;
    const name = 'web-' + new Date().toISOString().slice(0, 10) + '-' + Date.now().toString(36).slice(-4) + '.txt';
    await localFilePut(deck, name, text + '\n');
    toast('Saved ' + name);
    showFiles(deck);
  };
}

// ---- settings (batch size) — applies locally at once, syncs to server/device -
async function showSettings() {
  setHeader('Settings', { back: true });
  const main = $('main');
  main.classList.remove('no-scroll');
  const s = await getSettings();
  main.innerHTML = `
    <div class="group pad">
      <label class="setting-label" for="batchSizeInput">Cards per batch</label>
      <div class="setting-inline">
        <input type="number" id="batchSizeInput" min="${s.minBatchSize}" max="${s.maxBatchSize}"
               step="1" inputmode="numeric" value="${s.batchSize}">
        <span class="setting-range">${s.minBatchSize}&ndash;${s.maxBatchSize}</span>
      </div>
      <p class="sub" style="text-align:left; margin:10px 2px 0">Cards per study batch, here and on the device after its next sync.</p>
    </div>
    <button class="btn primary" id="settingsSave" type="button">Save</button>`;

  $('settingsSave').onclick = async () => {
    const btn = $('settingsSave');
    btn.disabled = true;
    try {
      const n = E.normalizeBatchSize(Number($('batchSizeInput').value));
      const cur = await getSettings();
      await kvPut('appSettings', { ...cur, batchSize: n, dirty: true });
      $('batchSizeInput').value = n; // snap to the clamped value
      for (const d of DECKS) await loadLocalDeck(d.id); // resize local batches right away
      toast('Settings saved');
      scheduleReconcile(); // push to server (and device on its next sync)
    } catch (e) {
      toast(e.message);
    } finally {
      btn.disabled = false;
    }
  };
}

// ---- PIN overlay (only needed to sync; local study never blocks) ----------
async function unlockWithPin() {
  const pin = $('tokenInput').value.trim();
  if (!pin) return;
  try {
    const res = await fetch('/api/auth', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ pin }),
    });
    if (!res.ok) {
      $('tokenInput').value = '';
      toast(res.status === 429 ? 'Too many attempts — try again in 15 min' : 'Wrong PIN');
      return;
    }
    setToken((await res.json()).token);
    hidePin();
    await bootstrapOrReconcile();
    route();
  } catch (e) {
    toast(e.message);
  }
}
function showPin() { $('tokenOverlay').classList.remove('hidden'); }
function hidePin() { $('tokenOverlay').classList.add('hidden'); }

// ---- boot -----------------------------------------------------------------
function registerSW() {
  if (!('serviceWorker' in navigator)) return;
  const hadController = !!navigator.serviceWorker.controller;
  navigator.serviceWorker.register('/sw.js').catch(() => {});
  navigator.serviceWorker.addEventListener('controllerchange', () => {
    if (hadController && !registerSW._reloaded) { registerSW._reloaded = true; location.reload(); }
  });
}

function wireEvents() {
  $('backBtn').onclick = () => history.back();
  $('tokenSave').onclick = unlockWithPin;
  $('tokenInput').addEventListener('keydown', (e) => { if (e.key === 'Enter') unlockWithPin(); });
  $('tokenSkip').onclick = () => { hidePin(); route(); };
  $('status').onclick = () => { if (!getToken() || $('status').dataset.kind === 'locked') showPin(); else reconcileAll('manual'); };
  window.addEventListener('online', () => reconcileAll('online'));
  window.addEventListener('offline', () => setStatus('offline'));
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && navigator.onLine && getToken()) reconcileAll('visible');
  });
}

// First online population, or a background refresh if we already have data.
async function bootstrapOrReconcile() {
  await reconcileAll('bootstrap');
}

async function boot() {
  registerSW();
  wireEvents();
  setStatus(navigator.onLine ? (getToken() ? 'online' : 'locked') : 'offline');

  const have = await anyLocalData();
  if (have) {
    route(); // render instantly from local cache (works offline)
    if (navigator.onLine && getToken()) reconcileAll('boot');
    else if (navigator.onLine && !getToken()) setStatus('locked');
    return;
  }
  // No cached data yet.
  if (!navigator.onLine) {
    setHeader('CrossPoint Cards');
    $('main').innerHTML = '<p class="sub" style="margin-top:40px">No offline data yet.<br>Connect to the internet once to download your decks, then it works offline.</p>';
    return;
  }
  if (!getToken()) { showPin(); return; }
  setStatus('syncing');
  try { await reconcileAll('first'); } catch (_) {}
  route();
}

boot();
