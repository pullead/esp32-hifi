'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const supertest = require('supertest');

process.env.NODE_ENV = 'development';
process.env.NCM_UPSTREAM_URL = 'http://upstream.invalid';
// Deliberately NOT setting DEVICE_API_KEY -- health must work without it.

const { createApp } = require('../src/server');

test('GET /esp/v1/health returns ok without a device key, and fast', async () => {
  const app = createApp();
  const res = await supertest(app).get('/esp/v1/health');
  assert.equal(res.status, 200);
  assert.equal(res.body.ok, true);
  assert.equal(res.body.service, 'esp32-ncm-gateway');
  assert.equal(typeof res.body.uptime_sec, 'number');
});
