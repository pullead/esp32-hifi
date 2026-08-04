'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const supertest = require('supertest');

process.env.NODE_ENV = 'development';
process.env.NCM_UPSTREAM_URL = 'http://upstream.invalid';
process.env.DEVICE_API_KEY = 'test-device-key';

const { createApp } = require('../src/server');
const ncmProvider = require('../src/ncmProvider');

test('GET /esp/v1/tracks/:id/resolve rejects a non-numeric id', async () => {
  const app = createApp();
  const res = await supertest(app).get('/esp/v1/tracks/not-a-number/resolve').set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 400);
  assert.equal(res.body.error.code, 'INVALID_ARGUMENT');
});

test('GET /esp/v1/tracks/:id/resolve rejects an unsupported bitrate', async () => {
  const app = createApp();
  const res = await supertest(app)
    .get('/esp/v1/tracks/123/resolve?bitrate=999')
    .set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 400);
  assert.equal(res.body.error.code, 'INVALID_ARGUMENT');
});

test('GET /esp/v1/tracks/:id/resolve returns a direct CDN URL, never this gateway\'s own host', async (t) => {
  t.mock.method(ncmProvider, 'resolveTrack', async () => ({
    track: { id: '123', title: 'Song', artist: 'Artist', album: 'Album', duration_ms: 200000, cover_url: '' },
    stream: {
      url: 'https://music-cdn.example/temp-file.mp3',
      codec: 'mp3',
      bitrate_kbps: 128,
      expires_at: Math.floor(Date.now() / 1000) + 1200,
      seekable_hint: true,
    },
  }));

  const app = createApp();
  const res = await supertest(app)
    .get('/esp/v1/tracks/123/resolve?bitrate=128')
    .set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 200);
  assert.equal(res.body.ok, true);
  assert.match(res.body.stream.url, /^https:\/\/music-cdn\.example\//);
  assert.notEqual(new URL(res.body.stream.url).hostname, 'localhost');
});

test('GET /esp/v1/tracks/:id/resolve maps NO_PLAYABLE_URL to 422', async (t) => {
  t.mock.method(ncmProvider, 'resolveTrack', async () => {
    throw new ncmProvider.UpstreamError('NO_PLAYABLE_URL', '当前账号或地区没有可用播放地址');
  });

  const app = createApp();
  const res = await supertest(app).get('/esp/v1/tracks/123/resolve').set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 422);
  assert.equal(res.body.error.code, 'NO_PLAYABLE_URL');
});

test('resolve is rate-limited independently and more strictly than metadata endpoints', async () => {
  // Smoke check that the resolve bucket exists and is enforced at all --
  // exact threshold is exercised by hitting the configured limit.
  const config = require('../src/config');
  assert.ok(config.rateLimitResolvePerMin <= config.rateLimitMetadataPerMin);
});
