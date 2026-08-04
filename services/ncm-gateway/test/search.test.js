'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const supertest = require('supertest');

process.env.NODE_ENV = 'development';
process.env.NCM_UPSTREAM_URL = 'http://upstream.invalid';
process.env.DEVICE_API_KEY = 'test-device-key';

const { createApp } = require('../src/server');
const ncmProvider = require('../src/ncmProvider');

test('GET /esp/v1/search without X-Device-Key is rejected', async () => {
  const app = createApp();
  const res = await supertest(app).get('/esp/v1/search?q=hello');
  assert.equal(res.status, 401);
  assert.equal(res.body.error.code, 'UNAUTHORIZED');
});

test('GET /esp/v1/search without q is a 400 INVALID_ARGUMENT', async () => {
  const app = createApp();
  const res = await supertest(app).get('/esp/v1/search').set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 400);
  assert.equal(res.body.error.code, 'INVALID_ARGUMENT');
});

test('GET /esp/v1/search returns the shape the firmware expects', async (t) => {
  t.mock.method(ncmProvider, 'search', async () => ({
    items: [
      {
        id: '123456',
        title: 'Song',
        artist: 'Artist',
        album: 'Album',
        duration_ms: 200000,
        cover_url: 'https://example.com/cover.jpg',
        playable_hint: true,
      },
    ],
    offset: 0,
    has_more: true,
  }));

  const app = createApp();
  const res = await supertest(app).get('/esp/v1/search?q=hello&limit=1').set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 200);
  assert.equal(res.body.ok, true);
  assert.equal(res.body.items.length, 1);
  assert.equal(res.body.items[0].id, '123456');
  assert.equal(res.body.has_more, true);
});

test('GET /esp/v1/search surfaces upstream failures as structured errors', async (t) => {
  t.mock.method(ncmProvider, 'search', async () => {
    throw new ncmProvider.UpstreamError('UPSTREAM_TIMEOUT', 'boom');
  });

  const app = createApp();
  const res = await supertest(app).get('/esp/v1/search?q=hello').set('X-Device-Key', 'test-device-key');
  assert.equal(res.status, 504);
  assert.equal(res.body.ok, false);
  assert.equal(res.body.error.code, 'UPSTREAM_TIMEOUT');
});
