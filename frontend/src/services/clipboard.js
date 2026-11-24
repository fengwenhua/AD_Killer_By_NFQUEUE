// Some root-manager WebViews do not expose the secure-context Clipboard API.
// The fallback is called only from a user's copy-button gesture.
export async function copyText(text) {
  try {
    if (globalThis.navigator?.clipboard?.writeText) {
      await globalThis.navigator.clipboard.writeText(text);
      return true;
    }
  } catch { /* Try the WebView's native selection-copy bridge. */ }
  const doc = globalThis.document;
  if (!doc?.body || typeof doc.execCommand !== 'function') return false;
  const focused = doc.activeElement, field = doc.createElement('textarea');
  field.value = text; field.readOnly = true;
  field.style.cssText = 'position:fixed;left:0;top:0;opacity:0;pointer-events:none';
  doc.body.append(field);
  try {
    field.focus({ preventScroll: true }); field.select();
    return doc.execCommand('copy') === true;
  } catch { return false; }
  finally {
    field.remove();
    if (focused?.isConnected) focused.focus({ preventScroll: true });
  }
}
