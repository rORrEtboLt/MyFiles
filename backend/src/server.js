import Hapi from '@hapi/hapi';
import Jwt from '@hapi/jwt';
import fs from 'node:fs/promises';
import { authRoutes } from './routes/auth.js';
import { syncRoutes } from './routes/sync.js';
import { imageRoutes } from './routes/images.js';
import { searchRoutes } from './routes/search.js';
import { labelRoutes } from './routes/labels.js';

export async function createServer({ config, pool }) {
  const server = Hapi.server({
    port: config.port,
    host: config.host,
    routes: {
      validate: {
        failAction: (request, h, err) => {
          throw err; // surface Joi details to the client instead of a bare 400
        },
      },
    },
  });

  await fs.mkdir(config.blobDir, { recursive: true });
  server.app.config = config;
  server.app.pool = pool;

  await server.register(Jwt);
  server.auth.strategy('jwt', 'jwt', {
    keys: { key: config.jwtSecret, algorithms: ['HS256'] },
    verify: { aud: false, iss: false, sub: false, nbf: true, exp: true, maxAgeSec: config.jwtTtlSec },
    validate: (artifacts) => ({
      isValid: Boolean(artifacts.decoded.payload.sub),
      credentials: { userId: artifacts.decoded.payload.sub },
    }),
  });
  server.auth.default('jwt');

  server.route({ method: 'GET', path: '/health', options: { auth: false }, handler: () => ({ ok: true }) });
  server.route([...authRoutes, ...syncRoutes, ...imageRoutes, ...searchRoutes, ...labelRoutes]);

  return server;
}
