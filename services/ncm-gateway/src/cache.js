'use strict';

// Minimal in-process TTL+LRU cache -- no external dependency, no
// persistence (see spec section 4.2: "不得依赖临时文件系统长期保存... 播放
// 状态", and Render's free-tier filesystem is ephemeral anyway). One
// process-lifetime Map per named cache (search/hot-playlists/playlist-
// detail/song-meta/lyrics); the process restarting (deploy, crash, cold
// start after sleep) just means an empty cache, which is fine for this data.

class TtlCache {
  constructor({ ttlMs, maxEntries = 500 }) {
    this.ttlMs = ttlMs;
    this.maxEntries = maxEntries;
    this.store = new Map(); // key -> { value, expiresAt }
  }

  get(key) {
    const entry = this.store.get(key);
    if (!entry) return undefined;
    if (Date.now() > entry.expiresAt) {
      this.store.delete(key);
      return undefined;
    }
    // Re-insert to mark as most-recently-used (Map preserves insertion order).
    this.store.delete(key);
    this.store.set(key, entry);
    return entry.value;
  }

  set(key, value) {
    if (this.store.has(key)) this.store.delete(key);
    else if (this.store.size >= this.maxEntries) {
      // Map iteration order is insertion order -> first key is the LRU one.
      const oldestKey = this.store.keys().next().value;
      this.store.delete(oldestKey);
    }
    this.store.set(key, { value, expiresAt: Date.now() + this.ttlMs });
  }

  size() {
    return this.store.size;
  }
}

// TTLs per spec section 4.5. Playback-URL resolves are intentionally never
// cached here -- the caller (ncmProvider.resolveTrack) always makes a fresh
// upstream request, since a stale temporary CDN URL served from cache would
// be indistinguishable from a working one until playback fails.
const searchCache = new TtlCache({ ttlMs: 8 * 60 * 1000, maxEntries: 300 });
const hotPlaylistsCache = new TtlCache({ ttlMs: 20 * 60 * 1000, maxEntries: 20 });
const playlistDetailCache = new TtlCache({ ttlMs: 10 * 60 * 1000, maxEntries: 100 });
const rankingsCache = new TtlCache({ ttlMs: 20 * 60 * 1000, maxEntries: 10 });
const newSongsCache = new TtlCache({ ttlMs: 10 * 60 * 1000, maxEntries: 20 });
const trackMetaCache = new TtlCache({ ttlMs: 45 * 60 * 1000, maxEntries: 500 });
const lyricsCache = new TtlCache({ ttlMs: 45 * 60 * 1000, maxEntries: 500 });

module.exports = {
  TtlCache,
  searchCache,
  hotPlaylistsCache,
  playlistDetailCache,
  rankingsCache,
  newSongsCache,
  trackMetaCache,
  lyricsCache,
};
