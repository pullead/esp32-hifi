'use strict';

const express = require('express');
const config = require('./config');
const espRoutes = require('./routes/espRoutes');

// Fail fast in production if the device auth secret isn't set -- an
// unauthenticated public gateway to a music-streaming resolve endpoint is
// exactly the kind of thing that must not silently boot open.
if (config.isProduction && !config.deviceApiKey) {
  // eslint-disable-next-line no-console
  console.error('FATAL: DEVICE_API_KEY must be set in production.');
  process.exit(1);
}
if (config.isProduction && !config.ncmUpstreamUrl) {
  // eslint-disable-next-line no-console
  console.error('FATAL: NCM_UPSTREAM_URL must be set in production.');
  process.exit(1);
}

function createApp() {
  const app = express();
  app.disable('x-powered-by');
  app.set('trust proxy', true); // Render sits behind its own proxy; needed for req.ip / rate limiting

  app.use('/esp/v1', espRoutes);

  app.get('/', (req, res) => {
    res.json({ ok: true, service: 'esp32-ncm-gateway', see: '/esp/v1/health' });
  });

  app.use((req, res) => {
    res.status(404).json({ ok: false, error: { code: 'NOT_FOUND', message: 'no such route' } });
  });

  // eslint-disable-next-line no-unused-vars
  app.use((err, req, res, next) => {
    // eslint-disable-next-line no-console
    console.error('[server] unhandled error', err);
    res.status(500).json({ ok: false, error: { code: 'UPSTREAM_UNAVAILABLE', message: 'internal error' } });
  });

  return app;
}

if (require.main === module) {
  const app = createApp();
  // Render requires binding 0.0.0.0 and its own PORT env var (spec 4.2 [1]).
  app.listen(config.port, '0.0.0.0', () => {
    // eslint-disable-next-line no-console
    console.log(`esp32-ncm-gateway listening on 0.0.0.0:${config.port} (${config.nodeEnv})`);
  });
}

module.exports = { createApp };
