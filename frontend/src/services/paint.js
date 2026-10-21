// Some Android bridges block JS while executing a native command. Flush Vue's
// DOM update and give the WebView an actual painted frame before invoking one.
export const afterPaint = () => typeof requestAnimationFrame === 'function'
  ? new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))
  : Promise.resolve();
