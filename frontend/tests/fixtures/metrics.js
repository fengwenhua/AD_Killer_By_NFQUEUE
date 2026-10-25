export function liveMetrics(overrides={}) {
  return {status:'current',owner_verified:true,owner_pid:123,owner_started:100,
    owner_boot:'12345678-1234-1234-1234-123456789abc',age_ms:0,updated_at:1,
    block_suffix_hits:4,block_exact_hits:3,...overrides};
}
