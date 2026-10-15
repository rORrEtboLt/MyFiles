import Joi from 'joi';
import { normalizeFileNumber } from '../lib/fileNumber.js';

// Online search across all synced documents. The phone has its own SQLite FTS5 index for offline use;
// this endpoint serves other clients (web, a second device that has not synced images yet, etc.).

export const searchRoutes = [
  {
    method: 'GET',
    path: '/api/search',
    options: {
      validate: { query: Joi.object({ q: Joi.string().trim().min(1).max(200).required(), limit: Joi.number().integer().min(1).max(100).default(25) }) },
    },
    handler: async (request) => {
      const { userId } = request.auth.credentials;
      const { q, limit } = request.query;
      const fileId = normalizeFileNumber(q);
      const { rows } = await request.server.app.pool.query(
        `WITH query AS (SELECT websearch_to_tsquery('simple', $2) AS tsq)
         SELECT d.id, d.title, d.tags, d.doc_date, f.file_number, f.title AS folder_title, f.location,
                ts_headline('simple', d.ocr_text, query.tsq, 'MaxWords=20, MinWords=8, MaxFragments=1') AS snippet,
                ts_rank(d.search, query.tsq) + CASE WHEN d.folder_id = $3 THEN 1 ELSE 0 END AS rank
           FROM documents d
           JOIN folders f ON f.user_id = d.user_id AND f.id = d.folder_id
           CROSS JOIN query
          WHERE d.user_id = $1 AND NOT d.deleted AND NOT f.deleted
            AND (d.search @@ query.tsq OR d.folder_id = $3 OR f.title ILIKE '%' || $2 || '%')
          ORDER BY rank DESC, d.updated_at DESC
          LIMIT $4`,
        [userId, q, fileId, limit],
      );
      return { results: rows };
    },
  },
];
