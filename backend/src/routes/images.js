import Boom from '@hapi/boom';
import Joi from 'joi';
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { withTransaction } from '../db.js';
import { lockUser } from './sync.js';
import { pdfFromJpegs } from '../lib/pdf.js';

// Page images are uploaded after the page row has been pushed. The file is written before the row's
// has_image flag flips (and its rev bumps), so any device that pulls has_image=true can download it.

const params = Joi.object({ id: Joi.string().max(64).pattern(/^[A-Za-z0-9_-]+$/).required() });

function sniffType(buf) {
  if (buf.length > 3 && buf[0] === 0xff && buf[1] === 0xd8 && buf[2] === 0xff) return 'image/jpeg';
  if (buf.length > 8 && buf.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))) return 'image/png';
  return null;
}

function blobPath(config, userId, pageId) {
  return path.join(config.blobDir, userId, pageId);
}

const safeName = (s) => s.replace(/[\\/:*?"<>|\r\n]+/g, '-').trim() || 'Document';

export const imageRoutes = [
  {
    // The document's pages as one PDF, built from the uploaded scans (same layout as the phone's PDF).
    method: 'GET',
    path: '/api/documents/{id}/pdf',
    options: { validate: { params } },
    handler: async (request, h) => {
      const { pool, config } = request.server.app;
      const { userId } = request.auth.credentials;
      const { rows: docs } = await pool.query(
        `SELECT d.title, f.file_number FROM documents d LEFT JOIN folders f ON f.user_id = d.user_id AND f.id = d.folder_id
          WHERE d.user_id = $1 AND d.id = $2 AND NOT d.deleted`,
        [userId, request.params.id],
      );
      if (docs.length === 0) throw Boom.notFound();
      const { rows: pages } = await pool.query(
        'SELECT id FROM pages WHERE user_id = $1 AND document_id = $2 AND NOT deleted AND has_image ORDER BY page_no',
        [userId, request.params.id],
      );
      const jpegs = [];
      for (const p of pages) {
        try {
          jpegs.push(await fs.readFile(blobPath(config, userId, p.id)));
        } catch (err) {
          if (err.code !== 'ENOENT') throw err;
        }
      }
      const result = pdfFromJpegs(jpegs);
      if (!result) throw Boom.notFound('No scanned pages uploaded for this document yet');
      const name = safeName(`${docs[0].file_number ?? ''} ${docs[0].title}`);
      return h
        .response(result.pdf)
        .type('application/pdf')
        .header('Content-Disposition', `inline; filename="${name.replace(/"/g, '')}.pdf"`)
        .header('X-Page-Count', String(result.pages));
    },
  },
  {
    method: 'PUT',
    path: '/api/pages/{id}/image',
    options: {
      validate: { params },
      payload: { parse: false, output: 'data', maxBytes: 15 * 1024 * 1024, allow: ['image/jpeg', 'image/png', 'application/octet-stream'] },
    },
    handler: async (request, h) => {
      const { pool, config } = request.server.app;
      const { userId } = request.auth.credentials;
      const { id } = request.params;
      const body = request.payload;
      if (!Buffer.isBuffer(body) || body.length > config.maxImageBytes) throw Boom.entityTooLarge();
      if (!sniffType(body)) throw Boom.unsupportedMediaType('Expected a JPEG or PNG image');

      const { rowCount } = await pool.query('SELECT 1 FROM pages WHERE user_id = $1 AND id = $2 AND NOT deleted', [userId, id]);
      if (rowCount === 0) throw Boom.notFound('Push the page record before uploading its image');

      const dest = blobPath(config, userId, id);
      await fs.mkdir(path.dirname(dest), { recursive: true });
      const tmp = `${dest}.${crypto.randomUUID()}.tmp`;
      await fs.writeFile(tmp, body);
      await fs.rename(tmp, dest);

      await withTransaction(pool, async (client) => {
        await lockUser(client, userId);
        await client.query(
          `UPDATE pages SET has_image = true, rev = nextval('sync_rev') WHERE user_id = $1 AND id = $2`,
          [userId, id],
        );
      });
      return h.response().code(204);
    },
  },
  {
    method: 'GET',
    path: '/api/pages/{id}/image',
    options: { validate: { params } },
    handler: async (request, h) => {
      const { config } = request.server.app;
      const { userId } = request.auth.credentials;
      let buf;
      try {
        buf = await fs.readFile(blobPath(config, userId, request.params.id));
      } catch (err) {
        if (err.code === 'ENOENT') throw Boom.notFound();
        throw err;
      }
      return h.response(buf).type(sniffType(buf) ?? 'application/octet-stream').header('Cache-Control', 'private, max-age=31536000, immutable');
    },
  },
];
