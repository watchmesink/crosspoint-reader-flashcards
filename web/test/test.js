'use strict';
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const engine = require('../engine');

let passed = 0;
function test(name, fn) {
  try {
    fn();
    passed++;
    console.log(`ok   ${name}`);
  } catch (err) {
    console.error(`FAIL ${name}`);
    console.error(err);
    process.exitCode = 1;
  }
}

// Optional fixtures: drop a real device progress bin + its deck txt into
// test/fixtures/ (gitignored — they contain personal study data) to also
// validate the codec against ground truth from actual hardware.
const realBinPath = path.join(__dirname, 'fixtures', 'flashcards_german.bin');
const sampleTxtPath = path.join(__dirname, 'fixtures', 'sample-deck.txt');
const realBin = fs.existsSync(realBinPath) ? fs.readFileSync(realBinPath) : null;
const sampleTxt = fs.existsSync(sampleTxtPath) ? fs.readFileSync(sampleTxtPath, 'utf8') : null;

if (realBin) {
  test('real device bin parses and re-serializes byte-identically', () => {
    const state = engine.parseProgressBin(realBin);
    assert.ok(state, 'parse failed');
    assert.strictEqual(state.version, 6);
    assert.ok(state.records.length > 0);
    const out = engine.serializeProgressBin(state);
    assert.ok(out.equals(realBin), `round-trip differs: ${out.length} vs ${realBin.length} bytes`);
  });
} else {
  console.log('skip real device bin round-trip (no test/fixtures/flashcards_german.bin)');
}

if (realBin && sampleTxt) {
  test('card hashes from txt match device progress records', () => {
    const { cards } = engine.parseDeckFiles([{ name: 'sample-deck.txt', content: sampleTxt }]);
    assert.ok(cards.length >= 5, `expected cards, got ${cards.length}`);
    const state = engine.parseProgressBin(realBin);
    const known = new Set(state.records.map((r) => r.key));
    for (const card of cards) {
      assert.ok(known.has(card.key), `key for "${card.prompt}" not found in device records`);
    }
  });
}

test('synthetic full state round-trips byte-identically twice', () => {
  const cards = mkCards(50);
  const state = engine.newDeckState();
  engine.selectNextCard(state, cards, true);
  const ratings = [engine.RATING.GOOD, engine.RATING.HARD, engine.RATING.EASY];
  for (let i = 0; i < 60; i++) {
    engine.rateCard(state, cards, state.currentKey, ratings[i % 3]);
  }
  engine.updateStudyStreak(state);
  const bin1 = engine.serializeProgressBin(state);
  const back = engine.parseProgressBin(bin1);
  assert.ok(back, 'parse failed');
  const bin2 = engine.serializeProgressBin(back);
  assert.ok(bin1.equals(bin2), 'second serialization differs');
  assert.strictEqual(back.reviewStep, state.reviewStep);
  assert.strictEqual(back.records.length, state.records.length);
});

test('parse rules: tab, last comma, comments, ascii trim, dedupe', () => {
  const content = [
    '# comment',
    '',
    'der Hund\tthe dog',
    'die Katze, der Kater,the cats', // last comma splits
    '  spaced \t  out  ',
    'der Hund\tthe dog', // duplicate
    'no separator line',
    ' nbsp\tkept', // NBSP must be preserved (ASCII-only trim)
  ].join('\n');
  const { cards, skippedLines, duplicateLines } = engine.parseDeckFiles([{ name: 'a.txt', content }]);
  assert.strictEqual(cards.length, 4);
  assert.strictEqual(cards[0].prompt, 'der Hund');
  assert.strictEqual(cards[1].prompt, 'die Katze, der Kater');
  assert.strictEqual(cards[1].answer, 'the cats');
  assert.strictEqual(cards[2].prompt, 'spaced');
  assert.strictEqual(cards[2].answer, 'out');
  assert.strictEqual(cards[3].prompt, ' nbsp');
  assert.strictEqual(duplicateLines, 1);
  assert.strictEqual(skippedLines, 1);
});

test('natural sort matches firmware comparator', () => {
  const names = ['file-10.txt', 'file-2.txt', 'File-1.txt', '10.02.26-1.txt', 'de-vocab.txt'];
  const sorted = [...names].sort(engine.naturalCompare);
  assert.deepStrictEqual(sorted, ['10.02.26-1.txt', 'de-vocab.txt', 'File-1.txt', 'file-2.txt', 'file-10.txt']);
});

function mkCards(n) {
  return Array.from({ length: n }, (_, i) => ({
    key: engine.hashCard(`prompt${i}`, `answer${i}`),
    prompt: `prompt${i}`,
    answer: `answer${i}`,
  }));
}

test('batch creation wraps and advances offset', () => {
  const cards = mkCards(30);
  const state = engine.newDeckState();
  engine.createNextBatch(state, cards);
  assert.strictEqual(state.batch.length, 20);
  assert.strictEqual(state.nextBatchStartOffset, 20);
  engine.createNextBatch(state, cards);
  assert.strictEqual(state.batch[0].key, cards[20].key);
  assert.strictEqual(state.batch[19].key, cards[9].key); // wrapped
  assert.strictEqual(state.nextBatchStartOffset, 10);
});

test('SM-2++ learning path: good, good, good graduates to review', () => {
  const cards = mkCards(25);
  const state = engine.newDeckState();
  engine.selectNextCard(state, cards, true);
  const key = state.currentKey;
  const rec = engine.findOrCreateRecord(state, key);

  engine.rateCard(state, cards, key, engine.RATING.GOOD);
  assert.strictEqual(rec.phase, engine.PHASE.LEARNING);
  assert.strictEqual(rec.learningStep, 1);
  assert.strictEqual(rec.dueStep, 1 + engine.LEARNING_STEPS[1]);

  engine.rateCard(state, cards, key, engine.RATING.GOOD);
  assert.strictEqual(rec.learningStep, 2);

  engine.rateCard(state, cards, key, engine.RATING.GOOD);
  assert.strictEqual(rec.phase, engine.PHASE.REVIEW);
  assert.ok(rec.interval >= 1);
  assert.strictEqual(rec.reviewCount, 3);
});

test('SM-2++ easy in learning graduates immediately with boosted ease', () => {
  const cards = mkCards(25);
  const state = engine.newDeckState();
  engine.restoreOrCreateBatch(state, cards);
  const key = cards[0].key;
  engine.rateCard(state, cards, key, engine.RATING.EASY);
  const rec = state.records.find((r) => r.key === key);
  assert.strictEqual(rec.phase, engine.PHASE.REVIEW);
  assert.strictEqual(rec.easeX100, 265);
  assert.ok(rec.interval >= 3 && rec.interval <= 5, `fuzzed around 4, got ${rec.interval}`);
  // batch entry marked processed only on EASY
  assert.strictEqual(state.batch.find((b) => b.key === key).processed, 1);
});

test('SM-2++ hard in review lapses to relearning, halves interval', () => {
  const cards = mkCards(25);
  const state = engine.newDeckState();
  const key = cards[0].key;
  const rec = engine.findOrCreateRecord(state, key);
  rec.phase = engine.PHASE.REVIEW;
  rec.interval = 10;
  rec.easeX100 = 250;
  engine.restoreOrCreateBatch(state, cards);
  engine.rateCard(state, cards, key, engine.RATING.HARD);
  assert.strictEqual(rec.phase, engine.PHASE.RELEARNING);
  assert.strictEqual(rec.lapses, 1);
  assert.strictEqual(rec.interval, 5);
  assert.strictEqual(rec.easeX100, 230);
  assert.strictEqual(rec.dueStep, state.reviewStep + engine.LEARNING_STEPS[0]);
  assert.strictEqual(state.batch.find((b) => b.key === key).processed, 0);
});

test('ease clamps at [130, 300]', () => {
  const state = engine.newDeckState();
  const rec = engine.newProgressRecord(1);
  rec.easeX100 = 135;
  engine.applySm2pp(state, rec, engine.RATING.HARD);
  assert.strictEqual(rec.easeX100, 130);
  rec.easeX100 = 295;
  rec.phase = engine.PHASE.REVIEW;
  rec.interval = 5;
  engine.applySm2pp(state, rec, engine.RATING.EASY);
  assert.strictEqual(rec.easeX100, 300);
});

test('interval fuzz is deterministic and bounded', () => {
  const state = engine.newDeckState();
  state.reviewStep = 884;
  const a = engine.applyIntervalFuzz(state, 100, 0xdeadbeef);
  const b = engine.applyIntervalFuzz(state, 100, 0xdeadbeef);
  assert.strictEqual(a, b);
  assert.ok(a >= 95 && a <= 105);
  assert.strictEqual(engine.applyIntervalFuzz(state, 2, 123), 2); // <=2 untouched
});

test('full batch study cycle: easy clears batch, new batch created', () => {
  const cards = mkCards(25);
  const state = engine.newDeckState();
  engine.selectNextCard(state, cards, true);
  for (let i = 0; i < 20; i++) {
    assert.ok(state.currentKey != null, `lost current card at step ${i}`);
    engine.rateCard(state, cards, state.currentKey, engine.RATING.EASY);
  }
  // 20 easies → batch completed and replaced with the next 20-card window
  assert.strictEqual(state.reviewStep, 20);
  assert.strictEqual(state.batch.filter((b) => b.processed).length, 0);
  assert.strictEqual(state.batch.length, 20);
  assert.ok(state.currentKey != null);
});

test('progress survives serialization round trip after rating', () => {
  const cards = mkCards(5);
  const state = engine.newDeckState();
  engine.selectNextCard(state, cards, true);
  engine.rateCard(state, cards, state.currentKey, engine.RATING.GOOD);
  engine.rateCard(state, cards, state.currentKey, engine.RATING.HARD);
  const bin = engine.serializeProgressBin(state);
  const back = engine.parseProgressBin(bin);
  assert.deepStrictEqual(
    back.records.map((r) => ({ ...r })),
    state.records.map((r) => ({ ...r })),
  );
  assert.strictEqual(back.reviewStep, state.reviewStep);
  assert.deepStrictEqual(back.batch, state.batch);
});

function buildStudiedState() {
  const cards = mkCards(40);
  const state = engine.newDeckState();
  engine.selectNextCard(state, cards, true);
  const ratings = [engine.RATING.GOOD, engine.RATING.HARD, engine.RATING.EASY];
  for (let i = 0; i < 30; i++) engine.rateCard(state, cards, state.currentKey, ratings[i % 3]);
  state.currentKey = null; // device bins carry no current-card notion
  return engine.serializeProgressBin(state);
}

test('merge is idempotent and keeps the more-reviewed record', () => {
  const baseBin = realBin || buildStudiedState();
  const same = engine.mergeDeckStates(engine.parseProgressBin(baseBin), engine.parseProgressBin(baseBin));
  assert.ok(engine.serializeProgressBin(same).equals(baseBin), 'merge(a,a) must equal a');

  const a = engine.parseProgressBin(baseBin);
  const b = engine.parseProgressBin(baseBin);
  // simulate extra studying on side b
  b.reviewStep += 5;
  b.records[0].reviewCount += 2;
  b.records[0].interval = 42;
  b.records[0].dueStep = b.reviewStep + 42;
  const merged = engine.mergeDeckStates(a, b);
  assert.strictEqual(merged.reviewStep, b.reviewStep);
  assert.strictEqual(merged.records[0].interval, 42);
  assert.strictEqual(merged.records[0].reviewCount, b.records[0].reviewCount);
  // merging again changes nothing
  const merged2 = engine.mergeDeckStates(merged, b);
  assert.ok(engine.serializeProgressBin(merged2).equals(engine.serializeProgressBin(merged)));
});

test('merge unions records from both sides', () => {
  const a = engine.newDeckState();
  const b = engine.newDeckState();
  engine.findOrCreateRecord(a, 111).reviewCount = 1;
  engine.findOrCreateRecord(b, 222).reviewCount = 2;
  const m = engine.mergeDeckStates(a, b);
  assert.strictEqual(m.records.length, 2);
});

test('streak increments on consecutive days, resets after a gap', () => {
  const day = 86400 * 1000;
  const state = engine.newDeckState();
  engine.updateStudyStreak(state, 100 * day);
  assert.strictEqual(state.streakDays, 1);
  engine.updateStudyStreak(state, 100 * day + 1000);
  assert.strictEqual(state.streakDays, 1); // same day
  engine.updateStudyStreak(state, 101 * day);
  assert.strictEqual(state.streakDays, 2);
  engine.updateStudyStreak(state, 105 * day);
  assert.strictEqual(state.streakDays, 1); // gap resets
});

test('restoreOrCreateBatch drops keys for removed cards', () => {
  const cards = mkCards(25);
  const state = engine.newDeckState();
  engine.createNextBatch(state, cards);
  const fewer = cards.slice(5); // first five cards removed
  engine.restoreOrCreateBatch(state, fewer);
  assert.strictEqual(state.batch.length, 15);
  assert.ok(state.batch.every((b) => fewer.some((c) => c.key === b.key)));
});

test('due scheduling prefers due cards, falls back to future', () => {
  const cards = mkCards(3);
  const state = engine.newDeckState();
  engine.createNextBatch(state, cards);
  for (const c of cards) {
    const r = engine.findOrCreateRecord(state, c.key);
    r.dueStep = 100; // all future
  }
  assert.strictEqual(engine.findNextCardIndex(state, cards, false), -1);
  engine.selectNextCard(state, cards, true);
  assert.ok(state.currentKey != null);
  state.records[1].dueStep = 0; // make one due
  state.currentKey = null;
  engine.selectNextCard(state, cards, false);
  assert.strictEqual(state.currentKey, cards[1].key);
});

console.log(`\n${passed} tests passed${process.exitCode ? ' (with failures)' : ''}`);
