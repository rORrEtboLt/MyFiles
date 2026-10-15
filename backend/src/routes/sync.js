import Boom from '@hapi/boom';
import Joi from 'joi';
import { normalizeFileNumber } from '../lib/fileNumber.js';
import { withTransaction } from '../db.js';

// Sync protocol
// -------------
// GET  /api/sync/pull?cursor=N   -> { cursor, hasMore, changes: { folders: [...], documents: [...], pages: [...] } }
//      Rows (including tombstones, deleted=true) whose server rev > cursor. Client upserts them and
//      stores the returned cursor. Repeat while hasMore.
// POST /api/sync/push            <- { changes: { folders: [...], documents: [...], pages: [...] } }
//      Upserts with last-write-wins on the client's updated_at. Rows that lose are reported back in
//      `stale`; the next pull delivers the winning version.
//
// Writes for a user take an exclusive per-user advisory lock and pulls take the shared one, so a pull
// never observes a rev gap left by an in-flight transaction (which would make the client skip rows).

const id = Joi.string().max(64).pattern(/^[A-Za-z0-9_-]+$/);
const ms = Joi.number().integer().min(0).max(8.64e15);
const text = (max) => Joi.string().allow('').max(max).default('');
const base = { id: id.required(), created_at: ms.required(), updated_at: ms.required(), deleted: Joi.boolean().default(false) };

export const TABLES = {
  folders: {
    columns: ['file_number', 'title', 'location', 'notes'],
    schema: Joi.object({ ...base, file_number: Joi.string().trim().min(1).max(64).required(), title: text(500), location: text(500), notes: text(10_000) }),
  },
  documents: {
    columns: ['folder_id', 'title', 'tags', 'doc_date', 'ocr_text'],
    schema: Joi.object({ ...base, folder_id: id.required(), title: text(500), tags: text(1000), doc_date: text(64), ocr_text: text(500_000) }),
  },
  pages: {
    // has_image is server-owned: set when the image upload lands, never by push.
    columns: ['document_id', 'page_no', 'ocr_text', 'width', 'height'],
    readOnly: ['has_image'],
    schema: Joi.object({
      ...base, document_id: id.required(), page_no: Joi.number().integer().min(1).max(100_000).default(1),
      ocr_text: text(100_000), width: Joi.number().integer().min(0).default(0), height: Joi.number().integer().min(0).default(0),
    }),
  },
};

const PULL_LIMIT = 500;
const SYNC_FIELDS = ['id', 'created_at', 'updated_at', 'deleted'];

export function lockUser(client, userId, { shared = false } = {}) {
  const fn = shared ? 'pg_advisory_xact_lock_shared' : 'pg_advisory_xact_lock';
  return client.query(`SELECT ${fn}(hashtextextended($1, 0))`, [userId]);
}

const pushPayload = Joi.object({
  changes: Joi.object(
    Object.fromEntries(Object.entries(TABLES).map(([name, t]) => [name, Joi.array().items(t.schema).max(5000).default([])])),
  ).required(),
});

async function upsert(client, table, userId, row) {
  const { columns } = TABLES[table];
  const cols = [...SYNC_FIELDS, ...columns];
  const values = [userId, ...cols.map((c) => row[c])];
  const placeholders = cols.map((_, i) => `$${i + 2}`).join(', ');
  const assignments = cols.filter((c) => c !== 'id').map((c) => `${c} = EXCLUDED.${c}`).join(', ');
  const res = await client.query(
    `INSERT INTO ${table} (user_id, ${cols.join(', ')}) VALUES ($1, ${placeholders})
     ON CONFLICT (user_id, id) DO UPDATE SET ${assignments}, rev = nextval('sync_rev')
     WHERE EXCLUDED.updated_at >= ${table}.updated_at
     RETURNING id`,
    values,
  );
  return res.rowCount === 1;
}

export const syncRoutes = [
  {
    method: 'GET',
    path: '/api/sync/pull',
    options: { validate: { query: Joi.object({ cursor: Joi.number().integer().min(0).default(0) }) } },
    handler: async (request) => {
      const { userId } = request.auth.credentials;
      const { cursor } = request.query;
      return withTransaction(request.server.app.pool, async (client) => {
        await lockUser(client, userId, { shared: true });
        // Fetch up to PULL_LIMIT from each table, then keep the globally-lowest PULL_LIMIT revs so the
        // returned cursor never jumps past a row that was not sent.
        const all = [];
        for (const [table, t] of Object.entries(TABLES)) {
          const cols = [...SYNC_FIELDS, ...t.columns, ...(t.readOnly ?? []), 'rev'];
          const { rows } = await client.query(
            `SELECT ${cols.join(', ')} FROM ${table} WHERE user_id = $1 AND rev > $2 ORDER BY rev LIMIT $3`,
            [userId, cursor, PULL_LIMIT],
          );
          for (const row of rows) all.push({ table, row });
        }
        all.sort((a, b) => a.row.rev - b.row.rev);
        const page = all.slice(0, PULL_LIMIT);
        const changes = Object.fromEntries(Object.keys(TABLES).map((t) => [t, []]));
        for (const { table, row } of page) {
          const { rev, ...rest } = row;
          changes[table].push(rest);
        }
        return {
          cursor: page.length ? page[page.length - 1].row.rev : cursor,
          hasMore: all.length > PULL_LIMIT,
          changes,
        };
      });
    },
  },
  {
    method: 'POST',
    path: '/api/sync/push',
    options: { validate: { payload: pushPayload }, payload: { maxBytes: 50 * 1024 * 1024 } },
    handler: async (request) => {
      const { userId } = request.auth.credentials;
      const { changes } = request.payload;

      for (const f of changes.folders) {
        if (f.id !== normalizeFileNumber(f.file_number)) {
          throw Boom.badRequest(`folder id "${f.id}" must equal normalized file number "${normalizeFileNumber(f.file_number)}"`);
        }
      }

      return withTransaction(request.server.app.pool, async (client) => {
        await lockUser(client, userId);
        const applied = {};
        const stale = {};
        for (const table of Object.keys(TABLES)) {
          applied[table] = 0;
          stale[table] = [];
          for (const row of changes[table]) {
            if (await upsert(client, table, userId, row)) applied[table] += 1;
            else stale[table].push(row.id);
          }
        }
        return { applied, stale };
      });
    },
  },
];
