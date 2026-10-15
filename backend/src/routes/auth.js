import Boom from '@hapi/boom';
import Jwt from '@hapi/jwt';
import Joi from 'joi';
import bcrypt from 'bcryptjs';

const credentials = Joi.object({
  email: Joi.string().email().lowercase().trim().max(254).required(),
  password: Joi.string().min(8).max(200).required(),
});

function issueToken(config, userId) {
  return Jwt.token.generate({ sub: userId }, { key: config.jwtSecret, algorithm: 'HS256' }, { ttlSec: config.jwtTtlSec });
}

export const authRoutes = [
  {
    method: 'POST',
    path: '/api/auth/register',
    options: { auth: false, validate: { payload: credentials } },
    handler: async (request, h) => {
      const { pool, config } = request.server.app;
      const { email, password } = request.payload;
      const hash = await bcrypt.hash(password, 12);
      const { rows } = await pool.query(
        'INSERT INTO users (email, password_hash) VALUES ($1, $2) ON CONFLICT (email) DO NOTHING RETURNING id',
        [email, hash],
      );
      if (rows.length === 0) throw Boom.conflict('Email already registered');
      return h.response({ token: issueToken(config, rows[0].id), userId: rows[0].id }).code(201);
    },
  },
  {
    method: 'POST',
    path: '/api/auth/login',
    options: { auth: false, validate: { payload: credentials } },
    handler: async (request) => {
      const { pool, config } = request.server.app;
      const { email, password } = request.payload;
      const { rows } = await pool.query('SELECT id, password_hash FROM users WHERE email = $1', [email]);
      const ok = rows.length > 0 && (await bcrypt.compare(password, rows[0].password_hash));
      if (!ok) throw Boom.unauthorized('Invalid email or password');
      return { token: issueToken(config, rows[0].id), userId: rows[0].id };
    },
  },
];
