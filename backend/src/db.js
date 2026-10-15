import pg from 'pg';

// bigint (epoch-ms timestamps, rev) as JS numbers; safe up to 2^53.
pg.types.setTypeParser(20, (v) => Number(v));

export function createPool(connectionString) {
  return new pg.Pool({ connectionString, max: 10 });
}

export async function withTransaction(pool, fn) {
  const client = await pool.connect();
  try {
    await client.query('BEGIN');
    const result = await fn(client);
    await client.query('COMMIT');
    return result;
  } catch (err) {
    await client.query('ROLLBACK');
    throw err;
  } finally {
    client.release();
  }
}
