import test from 'node:test';
import assert from 'node:assert/strict';
import { applicationNotes } from '../src/domain/apps.js';

test('successful application saves explain retained missing selections and shared identities', () => {
  const notes = applicationNotes({ uninstalled_count: 2, shared_package_count: 3 }, 'applied');
  assert.equal(notes.length, 2);
  assert.match(notes[0], /2.*保留选择/);
  assert.match(notes[1], /3.*一同受到名单影响/);
  assert.equal(applicationNotes({ uninstalled_count: 1 }, 'deferred').length, 1);
});

test('unconfirmed results and malformed counts never imply successful identity resolution', () => {
  for (const phase of ['pending', 'failed', 'unknown', 'superseded'])
    assert.deepEqual(applicationNotes({ uninstalled_count: 1, shared_package_count: 2 }, phase), []);
  for (const uninstalled_count of [-1, 0, 1.5, '2', 100001, Infinity, null])
    assert.deepEqual(applicationNotes({ uninstalled_count }, 'applied'), []);
  assert.deepEqual(applicationNotes(null, 'applied'), []);
});
