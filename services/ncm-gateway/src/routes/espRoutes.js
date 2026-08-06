'use strict';

const express = require('express');
const config = require('../config');
const { requireDeviceKey, rateLimit, sendError } = require('../auth');
const validators = require('../validators');
const ncmProvider = require('../ncmProvider');
const cache = require('../cache');

const router = express.Router();

const startedAt = Date.now();

// GET /esp/v1/health -- no auth, no upstream call (spec 4.2/5.1). Mounted
// on the router (not gated by requireDeviceKey below) so it stays reachable
// even if DEVICE_API_KEY is misconfigured -- Render's own health check and
// this device's "wake the service" probe both depend on this never 401ing.
router.get('/health', (req, res) => {
  res.json({
    ok: true,
    service: 'esp32-ncm-gateway',
    version: '1.0.0',
    uptime_sec: Math.floor((Date.now() - startedAt) / 1000),
    login_available: Boolean(config.ncmCookie),
  });
});

// Everything below requires the device key and is rate-limited.
router.use(requireDeviceKey);

function withValidation(handler) {
  return async function wrapped(req, res) {
    try {
      await handler(req, res);
    } catch (err) {
      if (err instanceof validators.ValidationError) {
        return sendError(res, 400, 'INVALID_ARGUMENT', err.message);
      }
      if (err instanceof ncmProvider.UpstreamError) {
        const statusByCode = {
          NOT_FOUND: 404,
          ACCESS_DENIED: 403,
          NO_PLAYABLE_URL: 422,
          UNSUPPORTED_FORMAT: 422,
          UPSTREAM_TIMEOUT: 504,
          UPSTREAM_UNAVAILABLE: 502,
        };
        return sendError(res, statusByCode[err.code] || 502, err.code, err.message);
      }
      // eslint-disable-next-line no-console
      console.error('[esp-routes] unhandled error', err);
      return sendError(res, 500, 'UPSTREAM_UNAVAILABLE', 'internal error');
    }
  };
}

function cacheKey(prefix, ...parts) {
  return `${prefix}:${parts.join(':')}`;
}

router.get(
  '/search',
  rateLimit('metadata', config.rateLimitMetadataPerMin),
  withValidation(async (req, res) => {
    const q = validators.requireSearchQuery(req.query.q);
    const limit = validators.parseLimit(req.query.limit, { max: 10, defaultValue: 8 });
    const offset = validators.parseOffset(req.query.offset);

    const key = cacheKey('search', q, limit, offset);
    let result = cache.searchCache.get(key);
    if (!result) {
      result = await ncmProvider.search(q, limit, offset);
      cache.searchCache.set(key, result);
    }
    res.json({ ok: true, ...result });
  })
);

router.get(
  '/playlists/hot',
  rateLimit('metadata', config.rateLimitMetadataPerMin),
  withValidation(async (req, res) => {
    const limit = validators.parseLimit(req.query.limit, { max: 20, defaultValue: 8 });
    const offset = validators.parseOffset(req.query.offset);

    const key = cacheKey('hot', limit, offset);
    let result = cache.hotPlaylistsCache.get(key);
    if (!result) {
      result = await ncmProvider.hotPlaylists(limit, offset);
      cache.hotPlaylistsCache.set(key, result);
    }
    res.json({ ok: true, ...result });
  })
);

router.get(
  '/playlists/:id',
  rateLimit('metadata', config.rateLimitMetadataPerMin),
  withValidation(async (req, res) => {
    const id = validators.requireNumericId(req.params.id, 'playlist_id');
    const limit = validators.parseLimit(req.query.limit, { max: 20, defaultValue: 20 });
    const offset = validators.parseOffset(req.query.offset);

    const key = cacheKey('playlist', id, limit, offset);
    let result = cache.playlistDetailCache.get(key);
    if (!result) {
      result = await ncmProvider.playlistDetail(id, limit, offset);
      cache.playlistDetailCache.set(key, result);
    }
    res.json({ ok: true, ...result });
  })
);

// GET /esp/v1/rankings -- song-ranking charts (id/name/cover/update_freq).
// Tapping a chart on the device then opens /playlists/:id with the chart's
// id, since rankings are playlists upstream.
router.get(
  '/rankings',
  rateLimit('metadata', config.rateLimitMetadataPerMin),
  withValidation(async (req, res) => {
    const limit = validators.parseLimit(req.query.limit, { max: 15, defaultValue: 8 });
    const offset = validators.parseOffset(req.query.offset);

    const key = cacheKey('rankings', limit, offset);
    let result = cache.rankingsCache.get(key);
    if (!result) {
      result = await ncmProvider.rankings(limit, offset);
      cache.rankingsCache.set(key, result);
    }
    res.json({ ok: true, ...result });
  })
);

// GET /esp/v1/new-songs -- newest-song arrivals, normalized to the same
// track-item shape as search results (so the firmware renders one track
// list style for both).
router.get(
  '/new-songs',
  rateLimit('metadata', config.rateLimitMetadataPerMin),
  withValidation(async (req, res) => {
    const limit = validators.parseLimit(req.query.limit, { max: 20, defaultValue: 20 });
    const offset = validators.parseOffset(req.query.offset);

    const key = cacheKey('newSongs', limit, offset);
    let result = cache.newSongsCache.get(key);
    if (!result) {
      result = await ncmProvider.newSongs(limit, offset);
      cache.newSongsCache.set(key, result);
    }
    res.json({ ok: true, ...result });
  })
);

router.get(
  '/tracks/:id/resolve',
  rateLimit('resolve', config.rateLimitResolvePerMin),
  withValidation(async (req, res) => {
    const id = validators.requireNumericId(req.params.id, 'track_id');
    const bitrate = validators.parseBitrate(req.query.bitrate);
    // Never cached (see cache.js's comment) -- always a fresh upstream call.
    const result = await ncmProvider.resolveTrack(id, bitrate, req.hostname);
    res.json({ ok: true, ...result });
  })
);

router.get(
  '/tracks/:id/lyrics',
  rateLimit('metadata', config.rateLimitMetadataPerMin),
  withValidation(async (req, res) => {
    const id = validators.requireNumericId(req.params.id, 'track_id');
    const key = cacheKey('lyrics', id);
    let result = cache.lyricsCache.get(key);
    if (!result) {
      result = await ncmProvider.lyrics(id);
      cache.lyricsCache.set(key, result);
    }
    res.json({ ok: true, ...result });
  })
);

module.exports = router;
