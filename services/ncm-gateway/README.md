# esp32-ncm-gateway

Thin metadata/URL-resolve gateway sitting between the ESP32-S3 HiFi player
firmware and a NeteaseCloudMusicApiEnhanced deployment. It never sees or
forwards audio bytes -- it returns trimmed JSON (search results, playlist
listings, lyrics) and short-lived direct-CDN stream URLs; the ESP32 connects
to the music CDN itself.

See `docs/DEV_LOG...` in the repo root for the full design spec this was
built from. The short version of the hard boundaries this service enforces:

- Never proxies, caches, or forwards audio content.
- Never accepts or forwards login/VIP-bypass/region-bypass parameters.
  `ENABLE_GENERAL_UNBLOCK`, `ENABLE_RANDOM_CN_IP`, and `ENABLE_PROXY` must
  all be `false` -- the service refuses to boot otherwise (see `config.js`).
- Only serves whatever the configured account (or anonymous access) is
  actually entitled to play. A track without a real accessible URL comes
  back as a structured `NO_PLAYABLE_URL`/`ACCESS_DENIED` error, never a
  workaround.

## Architecture: two services, not one

```
ESP32-S3  --HTTPS-->  esp32-ncm-gateway (this service)  --HTTP-->  NeteaseCloudMusicApiEnhanced  --> 网易云
                              |
                              +--> nothing else. No DB, no persistent storage.
```

This repo only contains `esp32-ncm-gateway` -- the thin wrapper. It expects
a **separately deployed** instance of
[NeteaseCloudMusicApiEnhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced)
reachable at `NCM_UPSTREAM_URL`. Kept as two services instead of bundling
that project in-process so that:

- The volatile, frequently-patched third-party API (spec's own risk note:
  it can break whenever NetEase changes something) is isolated behind one
  config value (`NCM_UPSTREAM_URL`) -- redeploying/relocating it never
  touches this service's code.
- This service's own dependency footprint, auth, rate limiting, and
  response shaping stay small, auditable, and stable regardless of what the
  upstream project's own churn looks like.

### Deploying the upstream NCM API

Deploy NeteaseCloudMusicApiEnhanced as its own Render (or any Node-capable)
web service, following its own README. On that deployment, set (its own env
var names, not this service's):

```
ENABLE_GENERAL_UNBLOCK=false
ENABLE_RANDOM_CN_IP=false
ENABLE_PROXY=false
```

This is the same boundary this gateway enforces on itself -- setting it on
both ends means a config mistake on either side still can't turn on
unblock/region-bypass/audio-proxy behavior. Point `NCM_UPSTREAM_URL` (this
service's env var) at whatever URL that deployment ends up at.

## Local development

```bash
npm install
cp .env.example .env   # then fill in DEVICE_API_KEY and NCM_UPSTREAM_URL
npm run dev
npm test
```

`DEVICE_API_KEY` is optional in development (`NODE_ENV != production`) so
`npm test`/`npm run dev` work without provisioning a real secret; production
boot refuses to start without one (see `server.js`).

## API

All endpoints except `/esp/v1/health` require an `X-Device-Key` header
matching `DEVICE_API_KEY`. Full request/response shapes and the error-code
table are in the design spec; summary:

| Method | Path | Auth | Notes |
|---|---|---|---|
| GET | `/esp/v1/health` | none | Never calls upstream. Fast, always available. |
| GET | `/esp/v1/search?q=&limit=&offset=` | device key | `limit` capped at 10. |
| GET | `/esp/v1/playlists/hot?limit=&offset=` | device key | `limit` capped at 20. |
| GET | `/esp/v1/playlists/:id?limit=&offset=` | device key | `limit` capped at 20. |
| GET | `/esp/v1/tracks/:id/resolve?bitrate=` | device key | Never cached. `bitrate` in `{96,128,192,320}`. |
| GET | `/esp/v1/tracks/:id/lyrics` | device key | LRC parsed into `{time_ms, text}[]`, capped at 200 lines. |

Rate limits (per device key, per minute): metadata endpoints
`RATE_LIMIT_METADATA_PER_MIN` (default 60), resolve
`RATE_LIMIT_RESOLVE_PER_MIN` (default 20, intentionally stricter).

## Caching

In-process TTL+LRU only (`src/cache.js`), no external cache/DB, no
persistent filesystem use -- a Render free-tier restart just means a cold
cache. `/tracks/:id/resolve` is never cached (a stale temporary CDN URL
served from cache would look identical to a working one until playback
actually fails).

## Files

- `src/server.js` -- Express app bootstrap, boot-time config validation.
- `src/config.js` -- all env vars read once, in one place.
- `src/auth.js` -- `X-Device-Key` check + per-device-key rate limiting.
- `src/cache.js` -- TTL+LRU cache, one instance per cached resource type.
- `src/validators.js` -- request parameter validation.
- `src/ncmProvider.js` -- all upstream-API-shape knowledge lives here; talks
  to `NCM_UPSTREAM_URL` and normalizes responses to this gateway's own
  shapes. If the upstream deployment's endpoint shapes differ from what's
  assumed here, this is the file to adjust.
- `src/routes/espRoutes.js` -- the `/esp/v1/*` HTTP routes.
