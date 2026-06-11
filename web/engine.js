'use strict';
// Exact port of the CrossPoint firmware flashcards engine
// (src/activities/flashcards/FlashcardsActivity.cpp, branch codex/flashcards-0.1.13-push).
// "Time" is the per-deck reviewStep counter, not wall clock. Card identity is the
// FNV-1a hash of "prompt\tanswer" UTF-8 bytes, so progress survives file renames.

const PROGRESS_VERSION = 6;
const MAX_FLASHCARDS_TOTAL = 900;
const BATCH_SIZE = 20;
const LEARNING_STEPS = [1, 8, 48];
const MATURE_INTERVAL = 21;
const MAX_INTERVAL = 4096;
const MIN_EASE_X100 = 130;
const MAX_EASE_X100 = 300;
const CARD_TEXT_SIZE_LARGE = 2;

const PHASE = { LEARNING: 0, RELEARNING: 1, REVIEW: 2 };
const RATING = { HARD: 0, GOOD: 1, EASY: 2 };

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// FNV-1a over UTF-8 bytes of prompt + '\t' + answer, uint32 wrap-around.
function hashCard(prompt, answer) {
  let h = 2166136261 >>> 0;
  const mix = (b) => { h = Math.imul(h ^ b, 16777619) >>> 0; };
  for (const b of Buffer.from(prompt, 'utf8')) mix(b);
  mix(9); // '\t'
  for (const b of Buffer.from(answer, 'utf8')) mix(b);
  return h >>> 0;
}

// Firmware trim() uses std::isspace on unsigned char (C locale): ASCII whitespace only.
// JS String.trim() also strips Unicode spaces, which would change card hashes.
const ASCII_SPACE = new Set([0x20, 0x09, 0x0a, 0x0b, 0x0c, 0x0d]);
function asciiTrim(s) {
  let start = 0;
  let end = s.length;
  while (start < end && ASCII_SPACE.has(s.charCodeAt(start))) start++;
  while (end > start && ASCII_SPACE.has(s.charCodeAt(end - 1))) end--;
  return s.slice(start, end);
}

// Port of the firmware natural-sort comparator (digit runs compared numerically,
// leading zeros skipped, otherwise case-insensitive byte compare).
function naturalCompare(str1, str2) {
  let i = 0, j = 0;
  const isDigit = (c) => c >= '0' && c <= '9';
  while (i < str1.length && j < str2.length) {
    if (isDigit(str1[i]) && isDigit(str2[j])) {
      while (str1[i] === '0') i++;
      while (str2[j] === '0') j++;
      let len1 = 0, len2 = 0;
      while (isDigit(str1[i + len1])) len1++;
      while (isDigit(str2[j + len2])) len2++;
      if (len1 !== len2) return len1 < len2 ? -1 : 1;
      for (let k = 0; k < len1; k++) {
        if (str1[i + k] !== str2[j + k]) return str1[i + k] < str2[j + k] ? -1 : 1;
      }
      i += len1;
      j += len2;
    } else {
      const c1 = str1[i].toLowerCase();
      const c2 = str2[j].toLowerCase();
      if (c1 !== c2) return c1 < c2 ? -1 : 1;
      i++;
      j++;
    }
  }
  if (i >= str1.length && j < str2.length) return -1;
  if (j >= str2.length && i < str1.length) return 1;
  return 0;
}

// Parse deck TXT files into the card list, mirroring loadAllFlashcards():
// natural-sorted file order, '#' comments, tab split else LAST comma, ASCII trim,
// dedupe by hash across files, hard cap at 900 cards.
function parseDeckFiles(files /* [{name, content}] */) {
  const sorted = [...files].sort((a, b) => naturalCompare(a.name, b.name));
  const cards = [];
  const seen = new Set();
  let skippedLines = 0;
  let duplicateLines = 0;
  let reachedLimit = false;
  let filesWithCards = 0;

  for (const file of sorted) {
    const countBefore = cards.length;
    for (const rawLine of file.content.split('\n')) {
      const line = asciiTrim(rawLine.replace(/\r/g, ''));
      if (!line || line[0] === '#') continue;
      let splitPos = line.indexOf('\t');
      if (splitPos === -1) splitPos = line.lastIndexOf(',');
      if (splitPos === -1) { skippedLines++; continue; }
      const prompt = asciiTrim(line.slice(0, splitPos));
      const answer = asciiTrim(line.slice(splitPos + 1));
      if (!prompt || !answer) { skippedLines++; continue; }
      const key = hashCard(prompt, answer);
      if (seen.has(key)) { duplicateLines++; continue; }
      if (cards.length >= MAX_FLASHCARDS_TOTAL) { reachedLimit = true; break; }
      seen.add(key);
      cards.push({ key, prompt, answer });
    }
    if (cards.length > countBefore) filesWithCards++;
    if (reachedLimit) break;
  }

  let statusMessage = `Loaded ${cards.length} cards from ${filesWithCards} files`;
  if (skippedLines > 0) statusMessage += ` | skipped ${skippedLines}`;
  if (duplicateLines > 0) statusMessage += ` | dup ${duplicateLines}`;
  if (reachedLimit) statusMessage += ' | limit reached';
  return { cards, statusMessage, skippedLines, duplicateLines, reachedLimit };
}

function newProgressRecord(key) {
  return {
    key,
    reviewCount: 0,
    hardCount: 0,
    goodCount: 0,
    easyCount: 0,
    phase: PHASE.LEARNING,
    learningStep: 0,
    lapses: 0,
    interval: 0,
    easeX100: 250,
    dueStep: 0,
  };
}

function newDeckState() {
  return {
    version: PROGRESS_VERSION,
    reviewStep: 0,
    nextBatchStartOffset: 0,
    records: [],
    batch: [], // [{key, processed}]
    streakDays: 0,
    lastStudyUnixDay: -1,
    currentKey: null, // web-only, not serialized into the device bin
  };
}

// ---- Device binary codec (/.crosspoint/flashcards_<deck>.bin, packed LE) ----

function parseProgressBin(buf) {
  try {
    let off = 0;
    const u8 = () => { const v = buf.readUInt8(off); off += 1; return v; };
    const u16 = () => { const v = buf.readUInt16LE(off); off += 2; return v; };
    const u32 = () => { const v = buf.readUInt32LE(off); off += 4; return v; };
    const i32 = () => { const v = buf.readInt32LE(off); off += 4; return v; };

    const version = u8();
    if (version !== 6 && version !== 5 && version !== 4) return null;
    const state = newDeckState();
    state.reviewStep = u32();
    state.nextBatchStartOffset = u16();

    const count = u16();
    if (count > 5000) return null;
    for (let i = 0; i < count; i++) {
      const r = newProgressRecord(u32());
      r.reviewCount = u16();
      r.hardCount = u16();
      r.goodCount = u16();
      r.easyCount = u16();
      r.phase = u8();
      r.learningStep = u8();
      r.lapses = u8();
      r.interval = u16();
      r.easeX100 = u16();
      r.dueStep = u32();
      if (r.phase > PHASE.REVIEW) r.phase = PHASE.LEARNING;
      if (r.learningStep >= LEARNING_STEPS.length) r.learningStep = LEARNING_STEPS.length - 1;
      r.interval = clamp(r.interval, 0, MAX_INTERVAL);
      r.easeX100 = clamp(r.easeX100, MIN_EASE_X100, MAX_EASE_X100);
      state.records.push(r);
    }

    const batchCount = u8();
    if (batchCount > BATCH_SIZE) return null;
    for (let i = 0; i < batchCount; i++) {
      const key = u32();
      const processed = u8() === 0 ? 0 : 1;
      state.batch.push({ key, processed });
    }

    if (version >= 5) u8(); // cardTextSize, fixed to LARGE by firmware
    if (version >= 6) {
      state.streakDays = u16();
      state.lastStudyUnixDay = i32();
    }
    if (off !== buf.length) return null;
    return state;
  } catch {
    return null;
  }
}

function serializeProgressBin(state) {
  const recordBytes = 23;
  const batch = state.batch.slice(0, BATCH_SIZE);
  const size = 1 + 4 + 2 + 2 + state.records.length * recordBytes + 1 + batch.length * 5 + 1 + 2 + 4;
  const buf = Buffer.alloc(size);
  let off = 0;
  const u8 = (v) => { buf.writeUInt8(v & 0xff, off); off += 1; };
  const u16 = (v) => { buf.writeUInt16LE(v & 0xffff, off); off += 2; };
  const u32 = (v) => { buf.writeUInt32LE(v >>> 0, off); off += 4; };
  const i32 = (v) => { buf.writeInt32LE(v | 0, off); off += 4; };

  u8(PROGRESS_VERSION);
  u32(state.reviewStep);
  u16(state.nextBatchStartOffset);
  u16(state.records.length);
  for (const r of state.records) {
    u32(r.key);
    u16(r.reviewCount);
    u16(r.hardCount);
    u16(r.goodCount);
    u16(r.easyCount);
    u8(r.phase);
    u8(r.learningStep);
    u8(r.lapses);
    u16(r.interval);
    u16(r.easeX100);
    u32(r.dueStep);
  }
  u8(batch.length);
  for (const b of batch) {
    u32(b.key);
    u8(b.processed ? 1 : 0);
  }
  u8(CARD_TEXT_SIZE_LARGE);
  u16(state.streakDays);
  i32(state.lastStudyUnixDay);
  return buf;
}

// ---- Scheduling (exact SM-2++ port) ----

function findOrCreateRecord(state, key) {
  let rec = state.records.find((r) => r.key === key);
  if (!rec) {
    rec = newProgressRecord(key);
    state.records.push(rec);
  }
  return rec;
}

function applyIntervalFuzz(state, baseInterval, key) {
  if (baseInterval <= 2) return baseInterval;
  const range = Math.max(1, Math.floor(baseInterval / 20));
  const mix = (key ^ Math.imul(state.reviewStep, 2654435761)) >>> 0;
  const delta = (mix % (range * 2 + 1)) - range;
  return clamp(baseInterval + delta, 1, MAX_INTERVAL);
}

function applySm2pp(state, p, rating) {
  const phase = p.phase;

  if (phase === PHASE.LEARNING || phase === PHASE.RELEARNING) {
    const relearning = phase === PHASE.RELEARNING;

    if (rating === RATING.HARD) {
      if (p.learningStep > 0) p.learningStep--;
      p.easeX100 = clamp(p.easeX100 - 15, MIN_EASE_X100, MAX_EASE_X100);
      p.dueStep = (state.reviewStep + LEARNING_STEPS[p.learningStep]) >>> 0;
      return;
    }

    if (rating === RATING.GOOD) {
      if (p.learningStep + 1 < LEARNING_STEPS.length) {
        p.learningStep++;
        p.dueStep = (state.reviewStep + LEARNING_STEPS[p.learningStep]) >>> 0;
        return;
      }
      p.phase = PHASE.REVIEW;
      p.learningStep = 0;
      if (p.interval === 0) p.interval = relearning ? 2 : 1;
      p.interval = applyIntervalFuzz(state, p.interval, p.key);
      p.dueStep = (state.reviewStep + Math.max(1, p.interval)) >>> 0;
      return;
    }

    // EASY graduates immediately
    p.phase = PHASE.REVIEW;
    p.learningStep = 0;
    p.easeX100 = clamp(p.easeX100 + 15, MIN_EASE_X100, MAX_EASE_X100);
    if (relearning) {
      const base = Math.max(3, Math.floor((Math.max(1, p.interval) * 3) / 2));
      p.interval = applyIntervalFuzz(state, base, p.key);
    } else {
      p.interval = applyIntervalFuzz(state, 4, p.key);
    }
    p.dueStep = (state.reviewStep + Math.max(1, p.interval)) >>> 0;
    return;
  }

  // REVIEW phase
  const interval = Math.max(1, p.interval);

  if (rating === RATING.HARD) {
    if (p.lapses < 255) p.lapses++;
    p.phase = PHASE.RELEARNING;
    p.learningStep = 0;
    p.interval = Math.max(1, Math.floor(interval / 2));
    p.easeX100 = clamp(p.easeX100 - 20, MIN_EASE_X100, MAX_EASE_X100);
    p.dueStep = (state.reviewStep + LEARNING_STEPS[0]) >>> 0;
    return;
  }

  if (rating === RATING.GOOD) {
    const ease = p.easeX100 / 100;
    const base = Math.max(interval + 1, Math.round(interval * ease));
    p.interval = applyIntervalFuzz(state, clamp(base, 1, MAX_INTERVAL), p.key);
    p.dueStep = (state.reviewStep + Math.max(1, p.interval)) >>> 0;
    return;
  }

  // EASY: bump ease first, then scale by extra 1.3
  p.easeX100 = clamp(p.easeX100 + 15, MIN_EASE_X100, MAX_EASE_X100);
  const ease = p.easeX100 / 100;
  const base = Math.max(interval + 2, Math.round(interval * ease * 1.3));
  p.interval = applyIntervalFuzz(state, clamp(base, 1, MAX_INTERVAL), p.key);
  p.dueStep = (state.reviewStep + Math.max(1, p.interval)) >>> 0;
}

function currentUnixDay(nowMs = Date.now()) {
  return Math.floor(nowMs / 1000 / 86400);
}

function updateStudyStreak(state, nowMs = Date.now()) {
  const currentDay = currentUnixDay(nowMs);
  if (currentDay < 0) return;
  if (state.lastStudyUnixDay < 0) {
    state.streakDays = 1;
    state.lastStudyUnixDay = currentDay;
    return;
  }
  if (currentDay === state.lastStudyUnixDay) return;
  if (currentDay === state.lastStudyUnixDay + 1) {
    if (state.streakDays < 65535) state.streakDays++;
  } else if (currentDay > state.lastStudyUnixDay + 1) {
    state.streakDays = 1;
  }
  state.lastStudyUnixDay = currentDay;
}

// ---- Batch handling ----

function createNextBatch(state, cards) {
  state.batch = [];
  if (cards.length === 0) return;
  const total = cards.length;
  const batchSize = Math.min(BATCH_SIZE, total);
  const start = state.nextBatchStartOffset % total;
  for (let i = 0; i < batchSize; i++) {
    state.batch.push({ key: cards[(start + i) % total].key, processed: 0 });
  }
  state.nextBatchStartOffset = (start + batchSize) % total;
}

function isBatchComplete(state) {
  return state.batch.length > 0 && state.batch.every((b) => b.processed !== 0);
}

function restoreOrCreateBatch(state, cards) {
  if (cards.length === 0) {
    state.batch = [];
    state.currentKey = null;
    return;
  }
  state.nextBatchStartOffset = state.nextBatchStartOffset % cards.length;
  const keyExists = new Set(cards.map((c) => c.key));
  const restored = [];
  for (const b of state.batch) {
    if (!keyExists.has(b.key)) continue;
    if (restored.some((r) => r.key === b.key)) continue;
    restored.push({ key: b.key, processed: b.processed ? 1 : 0 });
    if (restored.length >= BATCH_SIZE) break;
  }
  state.batch = restored;
  if (state.batch.length === 0 || isBatchComplete(state)) createNextBatch(state, cards);
}

function findNextCardIndex(state, cards, includeFutureCards) {
  if (cards.length === 0 || state.batch.length === 0) return -1;
  const total = cards.length;
  const currentIndex = state.currentKey == null ? -1 : cards.findIndex((c) => c.key === state.currentKey);
  const ringDistance = (index) => {
    if (currentIndex < 0 || currentIndex >= total) return index;
    let d = index - currentIndex;
    if (d <= 0) d += total;
    return d;
  };
  const recByKey = new Map(state.records.map((r) => [r.key, r]));

  let bestIndex = -1;
  let bestDue = 0xffffffff;
  for (const b of state.batch) {
    if (b.processed !== 0) continue;
    const cardIndex = cards.findIndex((c) => c.key === b.key);
    if (cardIndex < 0) continue;
    const rec = recByKey.get(b.key);
    const due = rec ? rec.dueStep : 0;
    if (!includeFutureCards && due > state.reviewStep) continue;
    if (bestIndex < 0 || due < bestDue || (due === bestDue && ringDistance(cardIndex) < ringDistance(bestIndex))) {
      bestIndex = cardIndex;
      bestDue = due;
    }
  }
  return bestIndex;
}

function selectNextCard(state, cards, includeFutureCards) {
  if (cards.length === 0) {
    state.currentKey = null;
    return;
  }
  if (state.batch.length === 0) restoreOrCreateBatch(state, cards);
  if (isBatchComplete(state)) createNextBatch(state, cards);
  const idx = findNextCardIndex(state, cards, includeFutureCards);
  state.currentKey = idx >= 0 ? cards[idx].key : null;
}

// Mirrors rateCurrentCard(): rating semantics are device buttons Hard/Good/Easy.
// A batch entry is only marked processed by an EASY rating.
function rateCard(state, cards, key, rating, nowMs = Date.now()) {
  const card = cards.find((c) => c.key === key);
  if (!card) return false;

  const rec = findOrCreateRecord(state, key);
  state.reviewStep = (state.reviewStep + 1) >>> 0;
  if (rec.reviewCount < 65535) rec.reviewCount++;
  if (rating === RATING.HARD && rec.hardCount < 65535) rec.hardCount++;
  if (rating === RATING.GOOD && rec.goodCount < 65535) rec.goodCount++;
  if (rating === RATING.EASY && rec.easyCount < 65535) rec.easyCount++;

  applySm2pp(state, rec, rating);
  updateStudyStreak(state, nowMs);

  state.currentKey = key; // ring distance pivots on the card just rated
  for (const b of state.batch) {
    if (b.key === key) {
      b.processed = rating === RATING.EASY ? 1 : 0;
      break;
    }
  }
  if (isBatchComplete(state)) createNextBatch(state, cards);

  selectNextCard(state, cards, false);
  if (state.currentKey == null) selectNextCard(state, cards, true);
  return true;
}

// ---- Stats / labels (parity with device UI) ----

function isCardMemorized(rec) {
  return rec.phase === PHASE.REVIEW && rec.interval > 0;
}

function countMemorized(state, cards) {
  const recByKey = new Map(state.records.map((r) => [r.key, r]));
  let n = 0;
  for (const c of cards) {
    const rec = recByKey.get(c.key);
    if (rec && isCardMemorized(rec)) n++;
  }
  return n;
}

function memorizationInfo(rec) {
  if (rec.phase === PHASE.LEARNING) return `Memory: Learning ${rec.learningStep + 1}/${LEARNING_STEPS.length}`;
  if (rec.phase === PHASE.RELEARNING) return `Memory: Relearning ${rec.learningStep + 1}/${LEARNING_STEPS.length}`;
  const stateLabel = rec.interval >= MATURE_INTERVAL ? 'Mature' : 'Young';
  const whole = Math.floor(rec.easeX100 / 100);
  const frac = String(rec.easeX100 % 100).padStart(2, '0');
  return `Memory: ${stateLabel} | EF ${whole}.${frac} | Ivl ${rec.interval}`;
}

// ---- Two-way merge for device sync ----
// The side with the larger reviewStep is "primary" (more recent activity); per-card
// records pick the more-reviewed side. Idempotent: merge(a, a) === a.

function mergeDeckStates(webState, deviceState) {
  if (!webState) return deviceState;
  if (!deviceState) return webState;

  const devicePrimary = deviceState.reviewStep >= webState.reviewStep;
  const primary = devicePrimary ? deviceState : webState;
  const secondary = devicePrimary ? webState : deviceState;

  const merged = newDeckState();
  merged.reviewStep = Math.max(webState.reviewStep, deviceState.reviewStep);
  merged.nextBatchStartOffset = primary.nextBatchStartOffset;
  merged.batch = primary.batch.map((b) => ({ key: b.key, processed: b.processed ? 1 : 0 }));
  merged.streakDays = Math.max(webState.streakDays, deviceState.streakDays);
  merged.lastStudyUnixDay = Math.max(webState.lastStudyUnixDay, deviceState.lastStudyUnixDay);
  merged.currentKey = devicePrimary ? null : (webState.currentKey ?? null);

  const secondaryByKey = new Map(secondary.records.map((r) => [r.key, r]));
  const primaryKeys = new Set();
  for (const pr of primary.records) {
    primaryKeys.add(pr.key);
    const sr = secondaryByKey.get(pr.key);
    merged.records.push({ ...pickRecord(pr, sr) });
  }
  for (const sr of secondary.records) {
    if (!primaryKeys.has(sr.key)) merged.records.push({ ...sr });
  }
  return merged;
}

function pickRecord(a, b) {
  if (!b) return a;
  if (a.reviewCount !== b.reviewCount) return a.reviewCount > b.reviewCount ? a : b;
  if (a.dueStep !== b.dueStep) return a.dueStep > b.dueStep ? a : b;
  return a;
}

module.exports = {
  PROGRESS_VERSION,
  MAX_FLASHCARDS_TOTAL,
  BATCH_SIZE,
  LEARNING_STEPS,
  MATURE_INTERVAL,
  PHASE,
  RATING,
  hashCard,
  asciiTrim,
  naturalCompare,
  parseDeckFiles,
  newProgressRecord,
  newDeckState,
  parseProgressBin,
  serializeProgressBin,
  findOrCreateRecord,
  applyIntervalFuzz,
  applySm2pp,
  updateStudyStreak,
  currentUnixDay,
  createNextBatch,
  isBatchComplete,
  restoreOrCreateBatch,
  findNextCardIndex,
  selectNextCard,
  rateCard,
  isCardMemorized,
  countMemorized,
  memorizationInfo,
  mergeDeckStates,
};
