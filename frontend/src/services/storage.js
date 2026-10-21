// Some manager file origins/privacy modes deny even the localStorage getter.
// Persistence is optional; the UI and current job observer must still work.
export function getStorage() {
  try { return globalThis.localStorage; } catch { return null; }
}
