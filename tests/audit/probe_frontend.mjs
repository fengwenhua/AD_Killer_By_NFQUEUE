// Opt-in characterization of CURRENT data-loss behavior, no native bridge.
import assert from 'node:assert/strict';
import { createApi, PATHS } from '../../frontend/src/services/api.js';
import { useCustomRules } from '../../frontend/src/composables/useCustomRules.js';

const host = { ksu: { exec(_command, _options, callback) {
  host[callback](1, '', 'permission denied');
} } };
const native = createApi(host);
let existing = 'keep.example.com\n';
let reloads = 0;
const api = {
  readFile: path => native.readFile(path),
  writeFile: async (path, text) => {
    assert.equal(path, PATHS.custom);
    existing = text;
    return { code: 0, stdout: '', stderr: '' };
  },
  command: async command => {
    assert.equal(command, '--reload');
    reloads++;
    return { code: 0, stdout: '{"status":"reloaded"}', stderr: '' };
  },
};
const editor = useCustomRules(api, { log() {}, toast() {} });
await editor.load();
assert.equal(editor.ready.value, true);
assert.equal(editor.black.value, '');
await editor.save();
assert.equal(existing, '');
assert.equal(reloads, 1);
console.log('OBSERVED read-failure-save: failed read marks editor ready and saving overwrites synthetic prior rules with empty text');
