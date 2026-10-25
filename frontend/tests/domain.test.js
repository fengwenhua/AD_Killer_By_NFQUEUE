import assert from 'node:assert/strict';
import test from 'node:test';
import { committedSourceOrder, HAGEZI_ID, isPackage, jobObservation, lastJson, lines, normalizeSource, overlap, parseSourceSummary, publishedSummary, sourceHistory, sourceSnapshot, sourcesDiffer, sourceUrlError, validJob } from '../src/domain/rules.js';

test('strict URL/package/job validation and JSON log parsing', () => {
  assert.equal(sourceUrlError('https://example.org/a'), '');
  assert.equal(sourceUrlError(HAGEZI_ID), '');
  for (const url of ['file:///tmp/a', 'ftp://example.org', 'https://user:pass@example.org', 'https://example.org/a\ncmd', 'https://example.org/\\x', 'invalid']) assert.ok(sourceUrlError(url), url);
  assert.ok(sourceUrlError('https://example.org', [{ id: 1, url: 'https://example.org' }]));
  assert.equal(sourceUrlError('https://example.org', [{ id: 1, url: 'https://example.org' }], 1), '');
  assert.equal(validJob('12-345'), true); assert.equal(validJob('12-345;id'), false);
  assert.equal(isPackage('com.example.app'), true); assert.equal(isPackage('com.example;id'), false);
  assert.deepEqual(lastJson('some log\n{"running":false}\ntrailing noise'), { running: false });
});

test('builtin identity and committed draft ordering, precise dirty detection', () => {
  assert.equal(normalizeSource({ url: HAGEZI_ID, enabled: false }, 0).locked, true);
  const rows = [normalizeSource({ url: 'https://off.org', enabled: false }, 0), normalizeSource({ url: 'https://on.org', enabled: true }, 1)];
  const saved = sourceSnapshot(rows), order = committedSourceOrder(rows);
  assert.deepEqual(order, [1, 0]); assert.equal(sourcesDiffer(rows, saved), false);
  rows[1].enabled = false; rows[0].enabled = true;
  assert.equal(sourcesDiffer(rows, saved), true); assert.deepEqual(order, [1, 0]);
  assert.deepEqual(committedSourceOrder(rows), [0, 1]);
  rows[1].enabled = true; rows[0].enabled = false;
  assert.equal(sourcesDiffer(rows, saved), false);
});

export const reportText = 'updated_at=1700000000\njob_id=123-456\npublish_status=published\ngeneration=999-111\nsource_count=2\naccepted_sources=1\nunique_total=2\nunique_white=1\nduplicate_lines=1\n\n[source_0]\nurl=https://example.org/a\nstatus=accepted\nchecked_at=1700000001\nrejected_unsupported=3\n\n[source_1]\nurl=https://example.org/b\nstatus=download_error\nreason=<img src=x>\n';

test('sync counts, history timestamps, stale publication rejection and non-HTML errors', () => {
  const report = parseSourceSummary(reportText);
  assert.equal(report.partial, true); assert.match(report.message, /来源 1\/2 成功/); assert.match(report.message, /过滤不支持或超长规则 3 行/);
  assert.match(report.message, /去重 1 行/); assert.equal(parseSourceSummary(''), null);
  assert.equal(publishedSummary(report, 'older log', '123-456'), null);
  assert.equal(publishedSummary(report, 'generation 999-111 已原子发布', 'wrong-job'), null);
  assert.equal(publishedSummary(report, 'generation 999-111 已原子发布', '123-456'), report);
  const record = sourceHistory({ url: 'https://example.org/a', enabled: true }, report.sources);
  assert.equal(record.label, '✓ 同步成功'); assert.equal(record.datetime, '2023-11-14T22:13:21.000Z');
  assert.match(sourceHistory({ url: 'https://example.org/b', enabled: true }, report.sources).detail, /<img src=x>/); // renderer must interpolate text, not v-html
  assert.match(sourceHistory({ url: 'https://example.org/a', enabled: false }, report.sources).label, /已停用/);
  report.sources[0].publish_status = 'failed';
  assert.match(sourceHistory({ url: 'https://example.org/a', enabled: true }, report.sources).label, /发布未确认/);
});

test('source cache fallback stays visible with its age; verified 304 and incomplete publication stay distinct', () => {
  const summary = parseSourceSummary('publish_status=published\nsource_count=1\naccepted_sources=1\n\n[source_0]\nurl=https://a.org\nstatus=accepted\nprovenance=cache_fallback\ncache_age_seconds=7200\ncache_verified_at=1700000000\nwarning=HTTP 503\n');
  assert.equal(summary.cached, 1); assert.equal(summary.partial, false); assert.match(summary.message, /使用缓存/);
  let history = sourceHistory({ url: 'https://a.org', enabled: true }, summary.sources);
  assert.equal(history.kind, 'warning'); assert.match(history.label, /使用缓存/); assert.match(history.detail, /2\.0 小时/); assert.match(history.detail, /HTTP 503/);
  for (const age of ['-1', '', 'NaN', '1e3']) {
    summary.sources[0].cache_age_seconds = age;
    assert.match(sourceHistory({ url: 'https://a.org', enabled: true }, summary.sources).detail, /年龄未核实/);
  }
  summary.sources[0].provenance = 'revalidated';
  history = sourceHistory({ url: 'https://a.org', enabled: true }, summary.sources);
  assert.equal(history.kind, 'success'); assert.match(history.detail, /304/);
  const incomplete = parseSourceSummary('publish_status=incomplete_sources\nsource_count=2\naccepted_sources=1\n');
  assert.match(incomplete.message, /本次未发布，保留整份旧规则/); assert.doesNotMatch(incomplete.message, /仅合并/);
});

test('custom line filtering and DNS overlap avoid dropped/duplicated lines', () => {
  assert.deepEqual(lines('# comment\n! comment\nads.example.com\n@@safe.example.org\n'), ['ads.example.com', '@@safe.example.org']);
  assert.equal(overlap(['a', 'b'], ['b', 'c']), 1);
  assert.equal(overlap(['x'], ['a']), 0);
});

test('backend exit codes distinguish terminal missing job from transient query failure', () => {
  assert.equal(jobObservation({ code: 1, stdout: '' }), null);
  assert.equal(jobObservation({ code: -1, stdout: '{"status":"running"}' }), null);
  for (const status of ['not_found', 'invalid_state', 'invalid_job']) assert.equal(jobObservation({ code: 1, stdout: JSON.stringify({ status }) }).status, status);
  assert.equal(jobObservation({ code: 0, stdout: '{"status":"failed","exit_code":2}' }).status, 'failed');
});
