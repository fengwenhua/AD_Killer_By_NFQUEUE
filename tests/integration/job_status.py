"""Read-only job observations; callers still require a bounded terminal result."""
import json
import subprocess


def read_job_state(binary, job_id, timeout=10):
    result = subprocess.run([str(binary), '--update-status', job_id], capture_output=True, text=True, timeout=timeout)
    state = json.loads(result.stdout)
    assert state.get('job_id') == job_id, (job_id, state)
    # An owner/lease can change between the journal read and identity query.
    # This is a retryable observation, never proof of success or permission to
    # resubmit work. Malformed records and other command failures still fail.
    assert ((result.returncode == 0 and state.get('status') in ('running', 'success', 'failed', 'interrupted')) or
            (result.returncode == 1 and state.get('status') == 'unverified')), (state, result.stderr)
    return state
