export const RELEASES_URL = 'https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases';
export function versionParts(value) {
  return typeof value === 'string' && /^\d{1,6}\.\d{1,6}(?:\.\d{1,6})?$/.test(value)
    ? value.split('.').map(Number).concat([0]).slice(0, 3) : null;
}
export function compareVersions(left, right) {
  const a = versionParts(left), b = versionParts(right);
  if (!a || !b) throw new Error('版本号格式无效');
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i] ? 1 : -1;
  return 0;
}
export function parseRelease(value) {
  if (!value || value.draft !== false || typeof value.prerelease !== 'boolean' ||
      typeof value.tag_name !== 'string' || !value.tag_name.startsWith('v')) return null;
  const version = value.tag_name.slice(1);
  if (!versionParts(version) || value.html_url !== `${RELEASES_URL}/tag/v${version}` || !Array.isArray(value.assets)) return null;
  const name = `AD_Killer_By_NFQUEUE-v${version}.zip`;
  const zipUrl = `${RELEASES_URL}/download/v${version}/${name}`;
  const zip = value.assets.find(asset => asset.name === name && asset.browser_download_url === zipUrl);
  const checksum = value.assets.find(asset => asset.name === name + '.sha256' && asset.browser_download_url === zipUrl + '.sha256');
  if (!zip || !checksum || !Number.isSafeInteger(zip.size) || zip.size < 4 || zip.size > 64*1024*1024) return null;
  return { version, preview: value.prerelease, url: value.html_url,
    notes: typeof value.body === 'string' ? value.body.slice(0, 8000) : '', size: zip.size };
}
export function selectRelease(response, preview) {
  if (response?.update) {
    const value = response.update, version = typeof value.version === 'string' ? value.version.replace(/^v/,'') : '';
    const base = `${RELEASES_URL}/download/v${version}/`;
    if (value.module !== 'AD_Killer_By_NFQUEUE' || !versionParts(version) ||
        !Number.isSafeInteger(value.versionCode) || value.versionCode < 1 ||
        !/^[a-f0-9]{64}$/.test(value.sha256 || '') ||
        value.zipUrl !== base+`AD_Killer_By_NFQUEUE-v${version}.zip` || value.changelog !== base+'changelog.md') return null;
    return {version,preview:false,url:`${RELEASES_URL}/tag/v${version}`,notes:typeof value.notes === 'string' ? value.notes.slice(0,8000) : '',size:null};
  }
  const values = Array.isArray(response) ? response : [response];
  const releases = values.map(parseRelease).filter(item => item && (preview || !item.preview));
  releases.sort((a,b) => compareVersions(b.version,a.version));
  return releases[0] || null;
}
