import Joi from 'joi';
import QRCode from 'qrcode';
import { displayFileNumber, qrPayload } from '../lib/fileNumber.js';

// Printable labels: a QR code (MYFILES:<FILE-NO>) next to the file number in large type, so both the
// app and a human can identify the envelope. Labels hold no private data, so they need no auth and can
// be opened straight in a browser and printed.

const escape = (s) => s.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

async function labelGroup(fileNumber, x, y) {
  const qr = await QRCode.toString(qrPayload(fileNumber), { type: 'svg', errorCorrectionLevel: 'M', margin: 1 });
  const inner = qr.replace('<svg', `<svg x="${x + 4}" y="${y + 4}" width="42" height="42"`);
  const label = displayFileNumber(fileNumber);
  const fontSize = Math.max(6, Math.min(16, 230 / Math.max(label.length, 1)));
  return `<g>
  <rect x="${x}" y="${y}" width="96" height="50" rx="3" fill="#fff" stroke="#000" stroke-width="0.4" stroke-dasharray="2 1"/>
  ${inner}
  <text x="${x + 50}" y="${y + 23}" font-family="Helvetica, Arial, sans-serif" font-size="5" fill="#444">FILE NO.</text>
  <text x="${x + 50}" y="${y + 23 + fontSize}" font-family="Helvetica, Arial, sans-serif" font-weight="700" font-size="${fontSize}">${escape(label)}</text>
</g>`;
}

export const labelRoutes = [
  {
    method: 'GET',
    path: '/api/labels/{fileNumber}.svg',
    options: { auth: false, validate: { params: Joi.object({ fileNumber: Joi.string().max(64).required() }) } },
    handler: async (request, h) => {
      const body = await labelGroup(request.params.fileNumber, 2, 2);
      return h
        .response(`<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="54mm" viewBox="0 0 100 54">${body}</svg>`)
        .type('image/svg+xml');
    },
  },
  {
    // A4 sheet of pre-numbered labels, e.g. /api/labels/sheet.svg?prefix=F-&from=1&count=10&pad=4
    method: 'GET',
    path: '/api/labels/sheet.svg',
    options: {
      auth: false,
      validate: {
        query: Joi.object({
          prefix: Joi.string().allow('').max(16).default('F-'),
          from: Joi.number().integer().min(0).default(1),
          count: Joi.number().integer().min(1).max(10).default(10),
          pad: Joi.number().integer().min(0).max(8).default(4),
        }),
      },
    },
    handler: async (request, h) => {
      const { prefix, from, count, pad } = request.query;
      const groups = [];
      for (let i = 0; i < count; i += 1) {
        const n = `${prefix}${String(from + i).padStart(pad, '0')}`;
        groups.push(await labelGroup(n, 8 + (i % 2) * 100, 10 + Math.floor(i / 2) * 55));
      }
      return h
        .response(`<svg xmlns="http://www.w3.org/2000/svg" width="210mm" height="297mm" viewBox="0 0 210 297">${groups.join('\n')}</svg>`)
        .type('image/svg+xml');
    },
  },
];
