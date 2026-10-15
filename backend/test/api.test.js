import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import pg from 'pg';
import { createPool } from '../src/db.js';
import { migrate } from '../src/migrate.js';
import { createServer } from '../src/server.js';
import { normalizeFileNumber } from '../src/lib/fileNumber.js';

const adminUrl = process.env.TEST_ADMIN_DATABASE_URL ?? 'postgres://localhost:5432/postgres';
const dbName = `myfiles_test_${process.pid}`;
let pool;
let server;
let blobDir;

// 2x3 RGB JPEG generated with Pillow (the PDF builder reads its size).
const REAL_JPEG = Buffer.from(
  'ffd8ffe000104a46494600010100000100010000ffdb0043000d090a0b0a080d0b0a0b0e0e0d0f13201513121213271c1e17202e2931302e292d2c333a4a3e333646372c2d405741464c4e525352323e5a615a50604a51524fffdb0043010e0e0e131113261515264f352d354f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4fffc00011080003000203012200021101031101ffc4001f0000010501010101010100000000000000000102030405060708090a0bffc400b5100002010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f0100030101010101010101010000000000000102030405060708090a0bffc400b51100020102040403040705040400010277000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f00f31a28a2988fffd9',
  'hex',
);
const JPEG = Buffer.from([0xff, 0xd8, 0xff, 0xe0, 0, 0x10, 0x4a, 0x46, 0x49, 0x46, 0, 1, 0xff, 0xd9]);

before(async () => {
  const admin = new pg.Client({ connectionString: adminUrl });
  await admin.connect();
  await admin.query(`CREATE DATABASE ${dbName}`);
  await admin.end();

  const url = new URL(adminUrl);
  url.pathname = `/${dbName}`;
  pool = createPool(url.toString());
  await migrate(pool);
  blobDir = await fs.mkdtemp(path.join(os.tmpdir(), 'myfiles-blobs-'));
  server = await createServer({
    pool,
    config: { port: 0, host: '127.0.0.1', jwtSecret: 'test-secret-test-secret-test-secret', jwtTtlSec: 3600, blobDir, maxImageBytes: 1024 * 1024 },
  });
  await server.initialize();
});

after(async () => {
  await server?.stop();
  await pool?.end();
  const admin = new pg.Client({ connectionString: adminUrl });
  await admin.connect();
  await admin.query(`DROP DATABASE IF EXISTS ${dbName}`);
  await admin.end();
  await fs.rm(blobDir, { recursive: true, force: true });
});

async function register(email) {
  const res = await server.inject({ method: 'POST', url: '/api/auth/register', payload: { email, password: 'correct horse' } });
  assert.equal(res.statusCode, 201, res.payload);
  return { authorization: `Bearer ${res.result.token}` };
}

const push = (headers, changes) => server.inject({ method: 'POST', url: '/api/sync/push', headers, payload: { changes } });
const pull = (headers, cursor = 0) => server.inject({ method: 'GET', url: `/api/sync/pull?cursor=${cursor}`, headers });

const folder = (fileNumber, extra = {}) => ({
  id: normalizeFileNumber(fileNumber),
  file_number: fileNumber, title: '', created_at: 1000, updated_at: 1000, ...extra,
});

test('auth: register, login, reject bad password and missing token', async () => {
  await register('a@example.com');
  const dup = await server.inject({ method: 'POST', url: '/api/auth/register', payload: { email: 'A@example.com', password: 'correct horse' } });
  assert.equal(dup.statusCode, 409);
  const ok = await server.inject({ method: 'POST', url: '/api/auth/login', payload: { email: 'a@example.com', password: 'correct horse' } });
  assert.equal(ok.statusCode, 200);
  const bad = await server.inject({ method: 'POST', url: '/api/auth/login', payload: { email: 'a@example.com', password: 'wrong password' } });
  assert.equal(bad.statusCode, 401);
  const anon = await pull({});
  assert.equal(anon.statusCode, 401);
});

test('sync: push then pull round-trips, cursor advances, tombstones propagate', async () => {
  const h = await register('sync@example.com');
  const res = await push(h, {
    folders: [folder('F-0001', { title: 'Insurance' })],
    documents: [{ id: 'd1', folder_id: 'F-1', title: 'Car policy', ocr_text: 'policy number 99812', created_at: 1000, updated_at: 1000 }],
    pages: [{ id: 'p1', document_id: 'd1', page_no: 1, created_at: 1000, updated_at: 1000 }],
  });
  assert.equal(res.statusCode, 200, res.payload);
  assert.deepEqual(res.result.applied, { folders: 1, documents: 1, pages: 1 });

  const first = await pull(h);
  assert.equal(first.result.changes.folders[0].title, 'Insurance');
  assert.equal(first.result.changes.pages[0].has_image, false);
  assert.equal(first.result.hasMore, false);
  const { cursor } = first.result;

  const empty = await pull(h, cursor);
  assert.equal(empty.result.changes.documents.length, 0);
  assert.equal(empty.result.cursor, cursor);

  await push(h, { documents: [{ id: 'd1', folder_id: 'F-1', title: 'Car policy', deleted: true, created_at: 1000, updated_at: 2000 }] });
  const after = await pull(h, cursor);
  assert.equal(after.result.changes.documents.length, 1);
  assert.equal(after.result.changes.documents[0].deleted, true);
});

test('sync: last write wins, older update reported as stale', async () => {
  const h = await register('lww@example.com');
  await push(h, { folders: [folder('B 7', { title: 'new', updated_at: 5000 })] });
  const res = await push(h, { folders: [folder('b-7', { title: 'old', updated_at: 4000 })] });
  assert.deepEqual(res.result.stale.folders, ['B-7']);
  const { result } = await pull(h);
  assert.equal(result.changes.folders.length, 1);
  assert.equal(result.changes.folders[0].title, 'new');
});

test('sync: folder id must be the normalized file number', async () => {
  const h = await register('norm@example.com');
  const res = await push(h, { folders: [{ ...folder('F-9'), id: 'something-else' }] });
  assert.equal(res.statusCode, 400);
});

test('sync: users are isolated', async () => {
  const a = await register('iso-a@example.com');
  const b = await register('iso-b@example.com');
  await push(a, { folders: [folder('SECRET-1')] });
  const { result } = await pull(b);
  assert.equal(result.changes.folders.length, 0);
});

test('sync: pull pages through more than one batch without losing rows', async () => {
  const h = await register('many@example.com');
  const docs = Array.from({ length: 700 }, (_, i) => ({ id: `d${i}`, folder_id: 'F-1', created_at: 1, updated_at: 1 }));
  await push(h, { folders: [folder('F-1')], documents: docs });
  let cursor = 0;
  let seen = 0;
  let rounds = 0;
  for (;;) {
    const { result } = await pull(h, cursor);
    seen += result.changes.documents.length + result.changes.folders.length;
    cursor = result.cursor;
    rounds += 1;
    if (!result.hasMore) break;
  }
  assert.equal(seen, 701);
  assert.equal(rounds, 2);
});

test('images: upload requires pushed page, flips has_image, downloads back', async () => {
  const h = await register('img@example.com');
  const early = await server.inject({ method: 'PUT', url: '/api/pages/p9/image', headers: { ...h, 'content-type': 'image/jpeg' }, payload: JPEG });
  assert.equal(early.statusCode, 404);

  await push(h, { folders: [folder('F-2')], documents: [{ id: 'd9', folder_id: 'F-2', created_at: 1, updated_at: 1 }], pages: [{ id: 'p9', document_id: 'd9', created_at: 1, updated_at: 1 }] });
  const { result: before } = await pull(h);

  const notImage = await server.inject({ method: 'PUT', url: '/api/pages/p9/image', headers: { ...h, 'content-type': 'image/jpeg' }, payload: Buffer.from('hello') });
  assert.equal(notImage.statusCode, 415);

  const up = await server.inject({ method: 'PUT', url: '/api/pages/p9/image', headers: { ...h, 'content-type': 'image/jpeg' }, payload: JPEG });
  assert.equal(up.statusCode, 204, up.payload);

  const { result: afterUpload } = await pull(h, before.cursor);
  assert.equal(afterUpload.changes.pages[0].has_image, true);

  const down = await server.inject({ method: 'GET', url: '/api/pages/p9/image', headers: h });
  assert.equal(down.statusCode, 200);
  assert.equal(down.headers['content-type'], 'image/jpeg');
  assert.deepEqual(down.rawPayload, JPEG);

  const other = await register('img-other@example.com');
  const stolen = await server.inject({ method: 'GET', url: '/api/pages/p9/image', headers: other });
  assert.equal(stolen.statusCode, 404);
});

test('pdf: document pages become one PDF, in page order, skipping pages without images', async () => {
  const h = await register('pdf@example.com');
  await push(h, {
    folders: [folder('P-1')],
    documents: [{ id: 'pd', folder_id: 'P-1', title: 'Deed', created_at: 1, updated_at: 1 }],
    pages: [1, 2, 3].map((n) => ({ id: `pp${n}`, document_id: 'pd', page_no: n, created_at: 1, updated_at: 1 })),
  });
  const none = await server.inject({ method: 'GET', url: '/api/documents/pd/pdf', headers: h });
  assert.equal(none.statusCode, 404);

  for (const id of ['pp1', 'pp3']) {
    const up = await server.inject({ method: 'PUT', url: `/api/pages/${id}/image`, headers: { ...h, 'content-type': 'image/jpeg' }, payload: REAL_JPEG });
    assert.equal(up.statusCode, 204);
  }
  const res = await server.inject({ method: 'GET', url: '/api/documents/pd/pdf', headers: h });
  assert.equal(res.statusCode, 200);
  assert.equal(res.headers['content-type'], 'application/pdf');
  assert.equal(res.headers['x-page-count'], '2');
  assert.match(res.headers['content-disposition'], /filename="P-1 Deed.pdf"/);
  const text = res.rawPayload.toString('latin1');
  assert.ok(text.startsWith('%PDF-1.4'));
  assert.ok(text.includes('/Count 2'));
  assert.ok(text.includes('/Width 2 /Height 3 /ColorSpace /DeviceRGB'));
  assert.ok(text.endsWith('%%EOF\n'));

  const other = await register('pdf-other@example.com');
  const stolen = await server.inject({ method: 'GET', url: '/api/documents/pd/pdf', headers: other });
  assert.equal(stolen.statusCode, 404);
});

test('search: full text over OCR, and lookup by file number', async () => {
  const h = await register('search@example.com');
  await push(h, {
    folders: [folder('F-0042', { title: 'House' }), folder('F-0043', { title: 'Car' })],
    documents: [
      { id: 's1', folder_id: 'F-42', title: 'Electricity bill', ocr_text: 'BESCOM consumer number 1234 amount due', created_at: 1, updated_at: 1 },
      { id: 's2', folder_id: 'F-43', title: 'Insurance', ocr_text: 'vehicle registration KA01', created_at: 1, updated_at: 1 },
    ],
  });
  const byText = await server.inject({ method: 'GET', url: '/api/search?q=bescom', headers: h });
  assert.equal(byText.statusCode, 200);
  assert.deepEqual(byText.result.results.map((r) => r.id), ['s1']);
  assert.equal(byText.result.results[0].file_number, 'F-0042'); // display form is preserved

  const byNumber = await server.inject({ method: 'GET', url: '/api/search?q=f%2043', headers: h });
  assert.deepEqual(byNumber.result.results.map((r) => r.id), ['s2']);
});

test('file numbers: handwritten variants normalize to one id', () => {
  for (const v of ['F-0042', 'f 42', ' F_042 ', 'F--42']) assert.equal(normalizeFileNumber(v), 'F-42');
  assert.equal(normalizeFileNumber('2024/007/B'), '2024-7-B');
  assert.equal(normalizeFileNumber('000'), '0');
});

test('labels: single label and sheet render QR + human readable number', async () => {
  const one = await server.inject({ method: 'GET', url: '/api/labels/f%2042.svg' });
  assert.equal(one.statusCode, 200);
  assert.match(one.headers['content-type'], /image\/svg\+xml/);
  assert.match(one.payload, />F 42</);
  assert.match(one.payload, /<path/);

  const sheet = await server.inject({ method: 'GET', url: '/api/labels/sheet.svg?prefix=B-&from=7&count=3&pad=3' });
  assert.equal(sheet.statusCode, 200);
  for (const n of ['B-007', 'B-008', 'B-009']) assert.match(sheet.payload, new RegExp(`>${n}<`));
});
