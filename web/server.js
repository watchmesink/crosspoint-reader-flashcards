'use strict';
// CrossPoint Flashcards web server — mirrors the Xteink X4 firmware flashcards
// (decks, SM-2++ scheduling, progress) and exposes a sync API for the device agent.
// Zero runtime dependencies; state lives as plain files under DATA_DIR.

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const engine = require('./engine');

const PORT = Number(process.env.PORT || 8080);
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, 'data');
const APP_SETTINGS_FILE = path.join(DATA_DIR, 'settings.json'); // global app settings (batch size)
const API_TOKEN = process.env.API_TOKEN || '';
const PIN = process.env.PIN || ''; // optional short unlock code for the browser UI
const PUBLIC_DIR = path.join(__dirname, 'public');

// PIN brute-force lockout: 5 wrong attempts per IP -> 15 min lock.
const pinAttempts = new Map(); // ip -> {fails, lockUntil}
const PIN_MAX_FAILS = 5;
const PIN_LOCK_MS = 15 * 60 * 1000;

const DECKS = [
  { id: 'german', label: 'German' },
  { id: 'ukrainian', label: 'Ukrainian' },
  { id: 'english', label: 'English' },
];
const DECK_IDS = new Set(DECKS.map((d) => d.id));

// ---- storage ----

function deckDir(deck) {
  return path.join(DATA_DIR, 'decks', deck);
}
function filesDir(deck) {
  return path.join(deckDir(deck), 'files');
}
function progressPath(deck) {
  return path.join(deckDir(deck), 'progress.json');
}

function ensureDirs() {
  for (const d of DECKS) fs.mkdirSync(filesDir(d.id), { recursive: true });
}

function atomicWrite(file, data) {
  const tmp = `${file}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, data);
  fs.renameSync(tmp, file);
}

function safeFileName(name) {
  const base = path.basename(String(name || ''));
  if (!base || base.startsWith('.') || base.includes('/') || base.includes('\\')) return null;
  if (!/\.txt$/i.test(base)) return null;
  if (base.length > 200) return null;
  return base;
}

function listDeckFiles(deck) {
  const dir = filesDir(deck);
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((n) => !n.startsWith('.') && /\.txt$/i.test(n))
    .map((name) => {
      const full = path.join(dir, name);
      const stat = fs.statSync(full);
      const content = fs.readFileSync(full);
      return {
        name,
        size: stat.size,
        mtime: stat.mtimeMs,
        sha256: crypto.createHash('sha256').update(content).digest('hex'),
      };
    })
    .sort((a, b) => engine.naturalCompare(a.name, b.name));
}

function readDeckFileContents(deck) {
  const dir = filesDir(deck);
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((n) => !n.startsWith('.') && /\.txt$/i.test(n))
    .map((name) => ({ name, content: fs.readFileSync(path.join(dir, name), 'utf8') }));
}

function loadState(deck) {
  const file = progressPath(deck);
  if (!fs.existsSync(file)) return null;
  try {
    const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
    const state = { ...engine.newDeckState(), ...raw };
    state.records = (raw.records || []).map((r) => ({ ...engine.newProgressRecord(r.key), ...r }));
    state.batch = (raw.batch || []).map((b) => ({ key: b.key, processed: b.processed ? 1 : 0 }));
    return state;
  } catch {
    return null;
  }
}

function saveState(deck, state, meta = {}) {
  const existingMeta = loadMeta(deck);
  const out = { ...state, meta: { ...existingMeta, ...meta } };
  atomicWrite(progressPath(deck), JSON.stringify(out, null, 1));
}

function loadMeta(deck) {
  const file = progressPath(deck);
  if (!fs.existsSync(file)) return {};
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8')).meta || {};
  } catch {
    return {};
  }
}

function addTombstone(deck, name) {
  const meta = loadMeta(deck);
  const tombstones = { ...(meta.tombstones || {}), [name]: new Date().toISOString() };
  saveState(deck, loadState(deck) || engine.newDeckState(), { tombstones });
}

function clearTombstone(deck, name) {
  const meta = loadMeta(deck);
  if (!meta.tombstones || !(name in meta.tombstones)) return;
  const tombstones = { ...meta.tombstones };
  delete tombstones[name];
  saveState(deck, loadState(deck) || engine.newDeckState(), { tombstones });
}

// ---- app settings (single global JSON at DATA_DIR/settings.json) ----
// Currently just the configurable study batch size; normalized on both read and
// write so a corrupt/out-of-range value can never reach the engine.

function defaultAppSettings() {
  return { batchSize: engine.BATCH_SIZE };
}

function normalizeAppSettings(raw) {
  return { batchSize: engine.normalizeBatchSize(raw && raw.batchSize) };
}

function loadAppSettings() {
  if (!fs.existsSync(APP_SETTINGS_FILE)) return defaultAppSettings();
  try {
    return normalizeAppSettings(JSON.parse(fs.readFileSync(APP_SETTINGS_FILE, 'utf8')));
  } catch {
    return defaultAppSettings();
  }
}

function saveAppSettings(settings) {
  fs.mkdirSync(DATA_DIR, { recursive: true });
  const normalized = normalizeAppSettings(settings);
  atomicWrite(APP_SETTINGS_FILE, JSON.stringify(normalized, null, 1));
  return normalized;
}

// Shape returned to clients: current value plus the allowed range for the UI.
function publicAppSettings() {
  const settings = loadAppSettings();
  return {
    batchSize: settings.batchSize,
    minBatchSize: engine.MIN_BATCH_SIZE,
    maxBatchSize: engine.BATCH_SIZE,
  };
}

function applyAppSettingsUpdate(body) {
  const settings = loadAppSettings();
  if (Object.prototype.hasOwnProperty.call(body, 'batchSize')) {
    settings.batchSize = engine.normalizeBatchSize(body.batchSize);
  }
  return saveAppSettings(settings);
}

// Load deck (cards + state), reconciling like the firmware's loadDeck() via the
// shared engine routine (same code path the offline browser client uses). The
// configured batch size flows through so a resized batch re-derives on open.
function openDeck(deck) {
  const settings = loadAppSettings();
  const { cards, state, statusMessage, changed } = engine.loadDeck(
    readDeckFileContents(deck),
    loadState(deck),
    settings.batchSize,
  );
  if (changed) saveState(deck, state);
  return { cards, state, statusMessage, settings };
}

function deckSummary(deckDef) {
  const { cards, state } = openDeck(deckDef.id);
  const meta = loadMeta(deckDef.id);
  return {
    deck: deckDef.id,
    label: deckDef.label,
    total: cards.length,
    memorized: engine.countMemorized(state, cards),
    streakDays: state.streakDays,
    reviewStep: state.reviewStep,
    dueNow: engine.countDue(state),
    files: listDeckFiles(deckDef.id).length,
    lastDeviceSyncAt: meta.lastDeviceSyncAt || null,
  };
}

function deckView(deckId) {
  const deckDef = DECKS.find((d) => d.id === deckId);
  const { cards, state, statusMessage, settings } = openDeck(deckId);
  const meta = loadMeta(deckId);
  return {
    deck: deckId,
    label: deckDef.label,
    total: cards.length,
    memorized: engine.countMemorized(state, cards),
    streakDays: state.streakDays,
    reviewStep: state.reviewStep,
    batchSize: state.batch.length, // actual live batch length (may be < configured for small decks)
    configuredBatchSize: settings.batchSize,
    batchProcessed: state.batch.filter((b) => b.processed).length,
    dueNow: engine.countDue(state),
    current: engine.projectCurrent(cards, state),
    statusMessage,
    lastDeviceSyncAt: meta.lastDeviceSyncAt || null,
  };
}

// ---- HTTP plumbing ----

function send(res, status, body, headers = {}) {
  const isBuf = Buffer.isBuffer(body);
  const data = isBuf ? body : typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': isBuf
      ? headers['Content-Type'] || 'application/octet-stream'
      : typeof body === 'string'
        ? 'text/plain; charset=utf-8'
        : 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
    ...headers,
  });
  res.end(data);
}

function readBody(req, limit = 8 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function authorized(req, url) {
  if (!API_TOKEN) return true;
  const header = req.headers.authorization || '';
  if (header === `Bearer ${API_TOKEN}`) return true;
  if (url.searchParams.get('token') === API_TOKEN) return true;
  return false;
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.webmanifest': 'application/manifest+json',
};

// Shell assets are revalidatable (not no-store) so the browser HTTP cache and the
// service worker can hold them for offline use; the SW handles update freshness.
function serveFileAt(res, file) {
  const ext = path.extname(file).toLowerCase();
  send(res, 200, fs.readFileSync(file), {
    'Content-Type': MIME[ext] || 'application/octet-stream',
    'Cache-Control': 'no-cache',
  });
}

function serveStatic(res, urlPath) {
  // engine.js lives at the project root (shared by server, tests, and the browser
  // PWA) — serve it here so the offline client can <script src="/engine.js"> the
  // exact same source the server runs.
  if (urlPath === '/engine.js') return serveFileAt(res, path.join(__dirname, 'engine.js'));

  let rel = urlPath === '/' ? '/index.html' : urlPath;
  const file = path.normalize(path.join(PUBLIC_DIR, rel));
  if (!file.startsWith(PUBLIC_DIR) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    return send(res, 404, 'Not found');
  }
  serveFileAt(res, file);
}

const RATINGS = { hard: engine.RATING.HARD, good: engine.RATING.GOOD, easy: engine.RATING.EASY };

async function handleApi(req, res, url) {
  const parts = url.pathname.split('/').filter(Boolean); // ['api', ...]

  if (parts[1] === 'health') return send(res, 200, { ok: true });

  // POST /api/auth {pin} -> {token} — lets the browser UI unlock with a short
  // PIN while scripts keep using the Bearer token directly.
  if (parts[1] === 'auth' && req.method === 'POST') {
    if (!PIN) return send(res, 404, { error: 'pin auth disabled (set PIN env var)' });
    const ip = String(req.headers['x-forwarded-for'] || req.socket.remoteAddress || '').split(',')[0].trim();
    const entry = pinAttempts.get(ip) || { fails: 0, lockUntil: 0 };
    if (Date.now() < entry.lockUntil) {
      return send(res, 429, { error: 'too many attempts, try again later' });
    }
    const body = JSON.parse((await readBody(req)).toString('utf8') || '{}');
    const pinOk =
      typeof body.pin === 'string' &&
      body.pin.length === PIN.length &&
      crypto.timingSafeEqual(Buffer.from(body.pin), Buffer.from(PIN));
    if (!pinOk) {
      entry.fails++;
      if (entry.fails >= PIN_MAX_FAILS) {
        entry.fails = 0;
        entry.lockUntil = Date.now() + PIN_LOCK_MS;
      }
      pinAttempts.set(ip, entry);
      return send(res, 401, { error: 'wrong pin' });
    }
    pinAttempts.delete(ip);
    return send(res, 200, { token: API_TOKEN });
  }

  if (!authorized(req, url)) return send(res, 401, { error: 'unauthorized' });

  // GET/POST /api/settings — global app settings (batch size). GET returns the
  // value plus its allowed range; POST clamps, persists, and re-reconciles every
  // deck's batch to the new size so it takes effect immediately.
  if (parts.length === 2 && parts[1] === 'settings') {
    if (req.method === 'GET') return send(res, 200, publicAppSettings());
    if (req.method === 'POST') {
      try {
        const body = JSON.parse((await readBody(req)).toString('utf8') || '{}');
        applyAppSettingsUpdate(body);
        for (const d of DECKS) openDeck(d.id); // resize live batches on disk right away
        return send(res, 200, publicAppSettings());
      } catch (err) {
        return send(res, 400, { error: String((err && err.message) || err) });
      }
    }
  }

  // GET /api/decks
  if (parts.length === 2 && parts[1] === 'decks' && req.method === 'GET') {
    return send(res, 200, DECKS.map(deckSummary));
  }

  const deck = parts[2];
  if (parts[1] === 'decks' && deck && !DECK_IDS.has(deck)) return send(res, 404, { error: 'unknown deck' });

  // GET /api/decks/:deck
  if (parts.length === 3 && parts[1] === 'decks' && req.method === 'GET') {
    return send(res, 200, deckView(deck));
  }

  // POST /api/decks/:deck/rate {key, rating}
  if (parts.length === 4 && parts[1] === 'decks' && parts[3] === 'rate' && req.method === 'POST') {
    const body = JSON.parse((await readBody(req)).toString('utf8') || '{}');
    const rating = RATINGS[String(body.rating || '').toLowerCase()];
    const key = Number(body.key) >>> 0;
    if (rating === undefined) return send(res, 400, { error: 'rating must be hard|good|easy' });
    const { cards, state, settings } = openDeck(deck);
    if (!engine.rateCard(state, cards, key, rating, Date.now(), settings.batchSize)) {
      return send(res, 404, { error: 'card not found' });
    }
    saveState(deck, state);
    return send(res, 200, deckView(deck));
  }

  // GET /api/decks/:deck/files
  if (parts.length === 4 && parts[1] === 'decks' && parts[3] === 'files' && req.method === 'GET') {
    return send(res, 200, listDeckFiles(deck));
  }

  // GET/PUT/DELETE /api/decks/:deck/files/:name
  if (parts.length === 5 && parts[1] === 'decks' && parts[3] === 'files') {
    const name = safeFileName(decodeURIComponent(parts[4]));
    if (!name) return send(res, 400, { error: 'invalid file name (.txt only)' });
    const file = path.join(filesDir(deck), name);
    if (req.method === 'GET') {
      if (!fs.existsSync(file)) return send(res, 404, { error: 'not found' });
      return send(res, 200, fs.readFileSync(file), { 'Content-Type': 'text/plain; charset=utf-8' });
    }
    if (req.method === 'PUT' || req.method === 'POST') {
      const body = await readBody(req);
      if (body.length > 1024 * 1024) return send(res, 413, { error: 'file too large' });
      atomicWrite(file, body);
      clearTombstone(deck, name);
      openDeck(deck); // reconcile records/batch with the new card set
      return send(res, 200, { ok: true, name, size: body.length });
    }
    if (req.method === 'DELETE') {
      if (fs.existsSync(file)) {
        fs.unlinkSync(file);
        addTombstone(deck, name); // so the device drops its copy instead of re-uploading
      }
      openDeck(deck);
      return send(res, 200, { ok: true });
    }
  }

  // GET /api/sync/:deck/manifest — file inventory + deletion tombstones, used
  // by the firmware's on-connect sync to reconcile /flashcards/<deck>/*.txt.
  if (parts.length === 4 && parts[1] === 'sync' && parts[3] === 'manifest' && req.method === 'GET') {
    if (!DECK_IDS.has(parts[2])) return send(res, 404, { error: 'unknown deck' });
    const meta = loadMeta(parts[2]);
    return send(res, 200, {
      files: listDeckFiles(parts[2]).map(({ name, size, sha256 }) => ({ name, size, sha256 })),
      tombstones: Object.keys(meta.tombstones || {}),
    });
  }

  // POST /api/sync/:deck/progress {bin: base64|null} -> {bin: base64|null}
  // The local agent sends the device's progress bin; we merge with web state,
  // adopt the merged result, and return it for writing back to the device.
  if (parts.length === 4 && parts[1] === 'sync' && parts[3] === 'progress' && req.method === 'POST') {
    if (!DECK_IDS.has(parts[2])) return send(res, 404, { error: 'unknown deck' });
    const syncDeck = parts[2];
    const body = JSON.parse((await readBody(req)).toString('utf8') || '{}');

    let deviceState = null;
    if (body.bin) {
      deviceState = engine.parseProgressBin(Buffer.from(String(body.bin), 'base64'));
      if (!deviceState) return send(res, 400, { error: 'unparseable progress bin' });
    }
    const webState = loadState(syncDeck);
    const merged = engine.mergeDeckStates(webState, deviceState);
    if (merged) saveState(syncDeck, merged, { lastDeviceSyncAt: new Date().toISOString() });
    else saveState(syncDeck, engine.newDeckState(), { lastDeviceSyncAt: new Date().toISOString() });

    // Don't push never-studied (pristine) state to the device — it would just
    // burn flash writes for decks that have no review history anywhere.
    const pristine =
      !merged || (merged.reviewStep === 0 && merged.records.every((r) => r.reviewCount === 0));
    const out = pristine ? null : engine.serializeProgressBin(merged).toString('base64');
    return send(res, 200, { bin: out });
  }

  // GET /api/decks/:deck/progress.bin (debug/backup)
  if (parts.length === 4 && parts[1] === 'decks' && parts[3] === 'progress.bin' && req.method === 'GET') {
    const state = loadState(deck);
    if (!state) return send(res, 404, { error: 'no progress' });
    return send(res, 200, engine.serializeProgressBin(state), {
      'Content-Type': 'application/octet-stream',
    });
  }

  return send(res, 404, { error: 'not found' });
}

ensureDirs();

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  try {
    if (url.pathname.startsWith('/api/')) return await handleApi(req, res, url);
    if (req.method === 'GET') return serveStatic(res, url.pathname);
    return send(res, 405, 'Method not allowed');
  } catch (err) {
    return send(res, 500, { error: String((err && err.message) || err) });
  }
});

server.listen(PORT, () => {
  console.log(`crosspoint-flashcards-web listening on :${PORT}, data dir ${DATA_DIR}, auth ${API_TOKEN ? 'on' : 'OFF'}`);
});
