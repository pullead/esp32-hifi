'use strict';

// All knowledge of the upstream NeteaseCloudMusicApiEnhanced deployment's
// actual endpoint shapes lives in this one file -- everything else in this
// service talks in terms of the normalized shapes defined below. When the
// upstream API changes (spec's own risk note: "可能因上游接口...变化而失
// 效"), this is the only file that should need editing.
//
// Endpoint paths/params here match the conventions documented by
// NeteaseCloudMusicApiEnhanced (https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced)
// as of this writing. If your deployed version differs, adjust the path
// constants below -- the normalization logic (buildSearchItem etc.) is
// written defensively (checks multiple possible field locations) so small
// upstream drift shouldn't need touching anything else.

const config = require('./config');

class UpstreamError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'UpstreamError';
    this.code = code; // one of the codes in the spec's error table
  }
}

function assertConfigured() {
  if (!config.ncmUpstreamUrl) {
    throw new UpstreamError('UPSTREAM_UNAVAILABLE', 'NCM_UPSTREAM_URL is not configured on this gateway');
  }
}

async function upstreamGet(path, params, timeoutMs) {
  assertConfigured();
  const url = new URL(config.ncmUpstreamUrl + path);
  for (const [key, value] of Object.entries(params || {})) {
    if (value === undefined || value === null || value === '') continue;
    url.searchParams.set(key, String(value));
  }
  // Deliberately never set here, no matter what a caller passes in `params`:
  // any unblock/region-spoof/proxy parameter. config.js already refuses to
  // boot if those flags are true, but this is the second, independent
  // enforcement point -- the actual place outbound requests are built.
  url.searchParams.delete('proxy');
  url.searchParams.delete('realIP');
  if (config.ncmCookie) url.searchParams.set('cookie', config.ncmCookie);

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs || config.upstreamTimeoutMs);
  let response;
  try {
    response = await fetch(url, { signal: controller.signal });
  } catch (err) {
    if (err.name === 'AbortError') {
      throw new UpstreamError('UPSTREAM_TIMEOUT', `upstream timeout calling ${path}`);
    }
    throw new UpstreamError('UPSTREAM_UNAVAILABLE', `upstream request failed: ${err.message}`);
  } finally {
    clearTimeout(timer);
  }
  if (!response.ok) {
    throw new UpstreamError('UPSTREAM_UNAVAILABLE', `upstream ${path} returned HTTP ${response.status}`);
  }
  try {
    return await response.json();
  } catch {
    throw new UpstreamError('UPSTREAM_UNAVAILABLE', `upstream ${path} returned non-JSON body`);
  }
}

function firstNonEmpty(...values) {
  for (const v of values) {
    if (v !== undefined && v !== null && v !== '') return v;
  }
  return undefined;
}

function joinArtists(artists) {
  if (!Array.isArray(artists)) return '';
  return artists.map((a) => a && a.name).filter(Boolean).join('/');
}

function buildSearchItem(song) {
  return {
    id: String(song.id),
    title: song.name || '',
    artist: joinArtists(song.artists || song.ar),
    album: firstNonEmpty(song.album && song.album.name, song.al && song.al.name, ''),
    duration_ms: firstNonEmpty(song.duration, song.dt, 0),
    cover_url: firstNonEmpty(song.album && song.album.picUrl, song.al && song.al.picUrl, ''),
    playable_hint: song.fee !== 1 && song.fee !== 4, // 1=VIP-only, 4=paid album -- not a bypass, just an honest hint
    vip: song.fee === 1,   // display-only badge: the device shows VIP/paid tags, never unlocks the track
    paid: song.fee === 4,
  };
}

async function search(query, limit, offset) {
  const data = await upstreamGet('/search', { keywords: query, type: 1, limit, offset });
  const songs = (data && data.result && data.result.songs) || [];
  const songCount = (data && data.result && data.result.songCount) || 0;
  const items = songs.map(buildSearchItem);
  // Backfill cover_url via /song/detail only for items the search response
  // didn't already include a cover for -- most deployments do include it,
  // so this is usually a no-op and avoids a second upstream round trip.
  const missingCover = items.filter((it) => !it.cover_url);
  if (missingCover.length > 0) {
    try {
      const detail = await upstreamGet('/song/detail', { ids: missingCover.map((it) => it.id).join(',') });
      const byId = new Map((detail.songs || []).map((s) => [String(s.id), s]));
      for (const it of missingCover) {
        const s = byId.get(it.id);
        if (s) it.cover_url = firstNonEmpty(s.al && s.al.picUrl, s.album && s.album.picUrl, '');
      }
    } catch {
      // Cover backfill is best-effort -- a missing cover_url is not a
      // reason to fail the whole search.
    }
  }
  return { items, offset, has_more: offset + items.length < songCount };
}

function buildPlaylistSummary(pl) {
  return {
    id: String(pl.id),
    name: pl.name || '',
    creator: firstNonEmpty(pl.creator && pl.creator.nickname, ''),
    cover_url: pl.coverImgUrl || '',
    track_count: pl.trackCount || 0,
  };
}

// cat is an optional NetEase playlist category tag ("华语"/"欧美"/"日语"/
// "粤语"/"韩语" ...) -- used by the device's language-classification
// category (语言分类). Empty means the default hot playlists feed.
async function hotPlaylists(limit, offset, cat) {
  const params = { limit, offset, order: 'hot' };
  if (cat) params.cat = cat;
  const data = await upstreamGet('/top/playlist', params);
  const playlists = (data && data.playlists) || [];
  const total = (data && data.total) || 0;
  return {
    items: playlists.map(buildPlaylistSummary),
    offset,
    has_more: offset + playlists.length < total,
    cat: cat || '',
  };
}

async function playlistDetail(playlistId, limit, offset) {
  const [detail, trackAll] = await Promise.all([
    upstreamGet('/playlist/detail', { id: playlistId }),
    upstreamGet('/playlist/track/all', { id: playlistId, limit, offset }),
  ]);
  if (!detail || !detail.playlist) {
    throw new UpstreamError('NOT_FOUND', 'playlist not found');
  }
  const pl = detail.playlist;
  const songs = (trackAll && trackAll.songs) || [];
  return {
    playlist: buildPlaylistSummary(pl),
    tracks: songs.map(buildSearchItem),
    offset,
    has_more: offset + songs.length < (pl.trackCount || 0),
  };
}

// Song rankings (歌曲排行榜). The upstream /toplist response already
// contains every chart's id/name/cover -- enough for the device to render a
// second-level chart picker. A chart's actual track list is served through
// the existing /esp/v1/playlists/:id endpoint (a ranking id IS a playlist
// id in NetEase's data model), so no separate track endpoint is needed.
async function rankings(limit, offset) {
  const data = await upstreamGet('/toplist', {});
  const list = (data && data.list) || [];
  const items = list.slice(offset, offset + limit).map((r) => ({
    id: String(r.id),
    name: r.name || '',
    cover_url: r.coverImgUrl || '',
    update_freq: r.updateFrequency || '',
  }));
  return { items, offset, has_more: offset + items.length < list.length };
}

// New-song arrivals (新歌速递) -- upstream /top/song is NetEase's "latest
// songs" feed. Its response shape varies between deployments (data as a
// bare song array vs. data.list), so both are handled defensively, same as
// buildSearchItem's own multi-location field lookups.
async function newSongs(limit, offset) {
  const data = await upstreamGet('/top/song', { type: 0, limit: limit + offset });
  const raw = data && data.data;
  const songs = Array.isArray(raw) ? raw : (raw && raw.list) || [];
  const total = songs.length;
  const items = songs.slice(offset, offset + limit).map(buildSearchItem);
  return { items, offset, has_more: offset + items.length < total };
}

// Wakes a sleeping Render free-tier upstream (api-enhanced) so the device's
// first browse after idle succeeds instead of 502ing. /top/playlist is the
// exact endpoint the device's hot-playlist page browses, so warming it also
// primes the gateway's hotPlaylistsCache. Uses a long timeout because a
// cold Render instance takes 30-90s to answer -- the device's wake task
// blocks on this endpoint until the upstream is genuinely ready.
async function wake() {
  await upstreamGet('/top/playlist', { limit: 1 }, 90000);
  return true;
}

// stream.url validation -- spec 5.4/11: must be a direct CDN URL, http(s)
// only, never this gateway's own host, never a playlist/manifest/HTML
// format. This is checked here (closest to where the URL enters the
// system) as well as being expected to be re-checked on the firmware side.
function assertPlayableStreamUrl(rawUrl, selfHost) {
  let parsed;
  try {
    parsed = new URL(rawUrl);
  } catch {
    throw new UpstreamError('NO_PLAYABLE_URL', 'resolved URL is not a valid absolute URL');
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new UpstreamError('NO_PLAYABLE_URL', 'resolved URL is not http(s)');
  }
  if (selfHost && parsed.hostname === selfHost) {
    throw new UpstreamError('NO_PLAYABLE_URL', 'resolved URL points back at this gateway, refusing to proxy');
  }
  const lowerPath = parsed.pathname.toLowerCase();
  if (lowerPath.endsWith('.m3u8') || lowerPath.endsWith('.mpd') || lowerPath.endsWith('.html') || lowerPath.endsWith('.htm')) {
    throw new UpstreamError('UNSUPPORTED_FORMAT', 'resolved URL is a manifest/HTML resource, not direct audio');
  }
}

async function resolveTrack(trackId, bitrateKbps, selfHost) {
  const [urlData, detailData] = await Promise.all([
    upstreamGet('/song/url', { id: trackId, br: bitrateKbps * 1000 }),
    upstreamGet('/song/detail', { ids: trackId }),
  ]);
  const entry = urlData && Array.isArray(urlData.data) ? urlData.data[0] : undefined;
  if (!entry || !entry.url) {
    // fee 1/4 without freeTrialInfo commonly means "needs purchase/VIP" --
    // surface as ACCESS_DENIED rather than the more generic NO_PLAYABLE_URL
    // so the UI copy can be accurate.
    const song = detailData && detailData.songs && detailData.songs[0];
    if (song && (song.fee === 1 || song.fee === 4)) {
      throw new UpstreamError('ACCESS_DENIED', 'track requires purchase or VIP access');
    }
    throw new UpstreamError('NO_PLAYABLE_URL', '当前账号或地区没有可用播放地址');
  }
  assertPlayableStreamUrl(entry.url, selfHost);

  const song = (detailData && detailData.songs && detailData.songs[0]) || {};
  return {
    track: {
      id: String(trackId),
      title: song.name || '',
      artist: joinArtists(song.ar),
      album: firstNonEmpty(song.al && song.al.name, ''),
      duration_ms: song.dt || 0,
      cover_url: firstNonEmpty(song.al && song.al.picUrl, ''),
    },
    stream: {
      url: entry.url,
      codec: (entry.type || 'mp3').toLowerCase(),
      bitrate_kbps: entry.br ? Math.round(entry.br / 1000) : bitrateKbps,
      // Netease's temporary CDN URLs don't come with a documented expiry
      // timestamp field on the classic /song/url endpoint -- +20 minutes is
      // a conservative planning value for the firmware's own
      // near-expiry re-resolve logic (spec 8.3), not a value read from the
      // upstream response.
      expires_at: Math.floor(Date.now() / 1000) + 20 * 60,
      seekable_hint: true,
    },
  };
}

// [mm:ss.xx] or [mm:ss.xxx] or [hh:mm:ss.xx] timestamp prefix.
const LRC_LINE_RE = /^\[(\d+):(\d+)(?:\.(\d+))?\](.*)$/;

function parseLrc(lrcText) {
  if (!lrcText) return { plain: '', synced: [] };
  const lines = lrcText.split(/\r?\n/);
  const synced = [];
  const plainLines = [];
  for (const line of lines) {
    const m = LRC_LINE_RE.exec(line.trim());
    if (!m) continue;
    const minutes = Number.parseInt(m[1], 10);
    const seconds = Number.parseInt(m[2], 10);
    const frac = m[3] ? Number.parseInt(m[3].padEnd(3, '0').slice(0, 3), 10) : 0;
    const text = m[4].trim();
    const timeMs = (minutes * 60 + seconds) * 1000 + frac;
    if (text) {
      synced.push({ time_ms: timeMs, text });
      plainLines.push(text);
    }
  }
  synced.sort((a, b) => a.time_ms - b.time_ms);
  return { plain: plainLines.join('\n'), synced };
}

// Spec 5.5: "超长歌词应截断". 200 synced lines covers essentially every real
// song; this bounds the response size sent to the (RAM-constrained) device.
const MAX_LYRIC_LINES = 200;

async function lyrics(trackId) {
  const data = await upstreamGet('/lyric', { id: trackId });
  if (!data || data.nolyric) return { plain: '', synced: [] };
  const parsed = parseLrc(data.lrc && data.lrc.lyric);
  if (parsed.synced.length > MAX_LYRIC_LINES) {
    parsed.synced = parsed.synced.slice(0, MAX_LYRIC_LINES);
  }
  return parsed;
}

module.exports = {
  UpstreamError,
  search,
  hotPlaylists,
  playlistDetail,
  rankings,
  newSongs,
  wake,
  resolveTrack,
  lyrics,
};
