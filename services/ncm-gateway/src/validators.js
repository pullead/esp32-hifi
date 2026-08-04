'use strict';

// Every value that reaches ncmProvider.js has gone through one of these --
// the device firmware is a fixed, known client, but this endpoint is
// public on the internet, so it gets validated like any other public API.

class ValidationError extends Error {
  constructor(message) {
    super(message);
    this.name = 'ValidationError';
  }
}

function parseQuery(raw, { min, max, defaultValue }) {
  if (raw === undefined || raw === '') return defaultValue;
  const n = Number.parseInt(String(raw), 10);
  if (!Number.isFinite(n)) throw new ValidationError('not a number');
  if (n < min || n > max) throw new ValidationError(`out of range [${min}, ${max}]`);
  return n;
}

function requireSearchQuery(q) {
  if (typeof q !== 'string' || q.length === 0) {
    throw new ValidationError('q is required');
  }
  const trimmed = q.trim();
  if (trimmed.length === 0) throw new ValidationError('q is empty');
  if (trimmed.length > 64) throw new ValidationError('q too long (max 64 chars)');
  return trimmed;
}

function requireNumericId(raw, fieldName = 'id') {
  if (typeof raw !== 'string' || !/^[0-9]{1,20}$/.test(raw)) {
    throw new ValidationError(`${fieldName} must be a numeric id`);
  }
  return raw;
}

function parseLimit(raw, { max = 20, defaultValue = 8 } = {}) {
  return parseQuery(raw, { min: 1, max, defaultValue });
}

function parseOffset(raw) {
  return parseQuery(raw, { min: 0, max: 100000, defaultValue: 0 });
}

// Only the bitrates this project's decode path is expected to handle well
// (spec: "第一版接受 MP3、AAC" / "128kbps default). 320 is offered for
// devices/accounts where it happens to be available; the upstream may still
// return a lower bitrate than requested if that's all the account/track
// has, which is not a validation failure on this side.
const ALLOWED_BITRATES = new Set([96, 128, 192, 320]);

function parseBitrate(raw) {
  if (raw === undefined || raw === '') return 128;
  const n = Number.parseInt(String(raw), 10);
  if (!ALLOWED_BITRATES.has(n)) {
    throw new ValidationError('bitrate must be one of 96, 128, 192, 320');
  }
  return n;
}

module.exports = {
  ValidationError,
  requireSearchQuery,
  requireNumericId,
  parseLimit,
  parseOffset,
  parseBitrate,
};
