'use strict';

// Central config -- every env var this service reads is listed here, once,
// so `grep NCM_` or `grep DEVICE_API_KEY` across the codebase turns up this
// file first. Nothing here should silently fall back to a "convenient"
// default that weakens the access-control boundary (device auth, the
// unblock/proxy flags) -- those either come from the environment or the
// service refuses to boot.

function parseBoolEnv(name, defaultValue) {
  const raw = process.env[name];
  if (raw === undefined || raw === '') return defaultValue;
  return raw.toLowerCase() === 'true' || raw === '1';
}

function parseIntEnv(name, defaultValue) {
  const raw = process.env[name];
  if (raw === undefined || raw === '') return defaultValue;
  const n = Number.parseInt(raw, 10);
  return Number.isFinite(n) ? n : defaultValue;
}

const nodeEnv = process.env.NODE_ENV || 'development';
const isProduction = nodeEnv === 'production';

const config = Object.freeze({
  nodeEnv,
  isProduction,
  port: parseIntEnv('PORT', 3000),

  // ESP32 -> this gateway auth (X-Device-Key header). Required in
  // production -- see server.js's boot check. Left optional in
  // development so `npm test`/`npm run dev` don't need a real secret.
  deviceApiKey: process.env.DEVICE_API_KEY || '',

  // Base URL of a *separately deployed* NeteaseCloudMusicApiEnhanced
  // instance (https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced).
  // This gateway is a thin wrapper around it, not a reimplementation --
  // see README.md for why it's deployed as its own service rather than
  // required in-process.
  ncmUpstreamUrl: (process.env.NCM_UPSTREAM_URL || '').replace(/\/+$/, ''),

  // Forwarded to the upstream API as its own `cookie` query param when
  // present (some endpoints -- personal playlists, some higher-bitrate
  // resolves -- need an authenticated session). Never logged, never sent
  // to the device. Optional: most search/hot-playlist/preview-bitrate
  // functionality works without it.
  ncmCookie: process.env.NCM_COOKIE || '',

  upstreamTimeoutMs: parseIntEnv('NCM_UPSTREAM_TIMEOUT_MS', 10000),

  // Hard product-boundary flags -- see docs/... spec: this project never
  // unlocks paid tracks, never bypasses region checks, never proxies audio.
  // These are read here so the gateway's own request-building code can
  // refuse to pass through any parameter that would ask the upstream for
  // that behavior, as defense in depth on top of configuring the upstream
  // deployment itself the same way (see README.md). They are NOT
  // request-time toggles -- there is no API surface that lets a caller turn
  // them on.
  enableGeneralUnblock: parseBoolEnv('ENABLE_GENERAL_UNBLOCK', false),
  enableRandomCnIp: parseBoolEnv('ENABLE_RANDOM_CN_IP', false),
  enableProxy: parseBoolEnv('ENABLE_PROXY', false),
  enableFlac: parseBoolEnv('ENABLE_FLAC', false),
  selectMaxBr: parseBoolEnv('SELECT_MAX_BR', false),

  // Rate limits (see auth.js) -- requests per minute, per device key.
  rateLimitMetadataPerMin: parseIntEnv('RATE_LIMIT_METADATA_PER_MIN', 60),
  rateLimitResolvePerMin: parseIntEnv('RATE_LIMIT_RESOLVE_PER_MIN', 20),
});

if (
  config.enableGeneralUnblock ||
  config.enableRandomCnIp ||
  config.enableProxy
) {
  // Fail loudly rather than silently running with a boundary disabled --
  // this is exactly the thing that must never be flipped on by a stray env
  // var typo on the Render dashboard.
  throw new Error(
    'config: ENABLE_GENERAL_UNBLOCK / ENABLE_RANDOM_CN_IP / ENABLE_PROXY must all be false. ' +
      'This gateway does not support unblocking, region-IP spoofing, or audio proxying.'
  );
}

module.exports = config;
