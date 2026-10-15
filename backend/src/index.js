import { config } from './config.js';
import { createPool } from './db.js';
import { migrate } from './migrate.js';
import { createServer } from './server.js';

const pool = createPool(config.databaseUrl);
await migrate(pool);
const server = await createServer({ config, pool });
await server.start();
console.log(`MyFiles API listening on ${server.info.uri}`);

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, async () => {
    await server.stop({ timeout: 5000 });
    await pool.end();
    process.exit(0);
  });
}
