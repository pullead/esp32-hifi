'use strict';

const config = require('./config');

// Sliding-window-ish per-minute counter, keyed by (bucket name + device
// key). In-process only -- fine for a single Render instance; if this ever
// runs with >1 instance the limit becomes per-instance, which is an
// acceptable degradation (still bounds load on the upstream NCM API) not a
// security hole.
const buckets = new Map(); // key -> { count, windowStartMs }

function checkRateLimit(bucketName, deviceKey, limitPerMin) {
  const key = `${bucketName}:${deviceKey}`;
  const now = Date.now();
  const windowMs = 60 * 1000;
  let entry = buckets.get(key);
  if (!entry || now - entry.windowStartMs >= windowMs) {
    entry = { count: 0, windowStartMs: now };
    buckets.set(key, entry);
  }
  entry.count += 1;
  return entry.count <= limitPerMin;
}

function sendError(res, httpStatus, code, message) {
  res.status(httpStatus).json({ ok: false, error: { code, message } });
}

// GET /esp/v1/health is intentionally exempt (spec 4.4: "健康检查：可免鉴权，
// 但不得调用网易上游") -- mounted before this middleware in server.js.
function requireDeviceKey(req, res, next) {
  const provided = req.header('X-Device-Key') || '';
  if (!config.deviceApiKey) {
    // Only reachable in development (server.js refuses to boot in
    // production without a real key) -- lets `npm run dev` work without
    // provisioning a secret, while production always enforces this.
    return next();
  }
  if (!provided || provided !== config.deviceApiKey) {
    return sendError(res, 401, 'UNAUTHORIZED', '设备 API Key 错误');
  }
  return next();
}

function rateLimit(bucketName, limitPerMin) {
  return function rateLimitMiddleware(req, res, next) {
    const deviceKey = req.header('X-Device-Key') || req.ip;
    if (!checkRateLimit(bucketName, deviceKey, limitPerMin)) {
      return sendError(res, 429, 'RATE_LIMITED', '请求过于频繁');
    }
    return next();
  };
}

module.exports = { requireDeviceKey, rateLimit, sendError };
