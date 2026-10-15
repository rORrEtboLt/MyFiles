import path from 'node:path';

const env = process.env;

export const config = {
  port: Number(env.PORT ?? 4000),
  host: env.HOST ?? '0.0.0.0',
  databaseUrl: env.DATABASE_URL ?? 'postgres://localhost:5432/myfiles',
  jwtSecret: env.JWT_SECRET ?? 'dev-only-change-me-dev-only-change-me',
  jwtTtlSec: Number(env.JWT_TTL_SEC ?? 60 * 60 * 24 * 90),
  blobDir: path.resolve(env.BLOB_DIR ?? 'blobs'),
  maxImageBytes: Number(env.MAX_IMAGE_BYTES ?? 15 * 1024 * 1024),
};

if (env.NODE_ENV === 'production' && config.jwtSecret.startsWith('dev-only')) {
  throw new Error('JWT_SECRET must be set in production');
}
