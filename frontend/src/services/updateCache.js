// Store only validated GitHub responses; caches are hints, never install receipts.
export const NOTES_CACHE_KEY = 'ad-killer-by-nfqueue-release-notes-cache';
export const CHECK_CACHE_KEY = 'ad-killer-by-nfqueue-release-check-cache';
export const NOTES_CACHE_TTL = 24 * 60 * 60 * 1000;
export const CHECK_CACHE_TTL = 6 * 60 * 60 * 1000;
const MAX_CACHE_CHARS = 65536;

export function dropCache(storage, key) {
  try { storage?.removeItem(key); } catch { /* storage is optional */ }
}
export function readCache(storage, key, ttl, decode, context = '', now = Date.now()) {
  try {
    const raw = storage?.getItem(key);
    if (!raw) return null;
    if (raw.length > MAX_CACHE_CHARS) throw Error('oversized cache');
    const record = JSON.parse(raw);
    if (record.schema !== 1 || record.context !== context || !Number.isSafeInteger(record.savedAt) ||
        record.savedAt < 0 || record.savedAt > now || now - record.savedAt > ttl) throw Error('expired/incompatible cache');
    const value = decode(record.response);
    if (!value) throw Error('invalid cached response');
    return { value, savedAt: record.savedAt };
  } catch { dropCache(storage, key); return null; }
}
export function writeCache(storage, key, response, context = '', now = Date.now()) {
  try {
    const raw = JSON.stringify({ schema: 1, savedAt: now, context, response });
    if (raw.length <= MAX_CACHE_CHARS) storage?.setItem(key, raw);
  } catch { /* preserve current session when storage is unavailable/full */ }
}
