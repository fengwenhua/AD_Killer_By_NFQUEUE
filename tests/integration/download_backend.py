#!/usr/bin/env python3
"""Record exec backend selection; only curl dispatches a real request."""
import json
import os
from pathlib import Path
import sys


backend = Path(sys.argv[0]).name.removeprefix("backend-")
with open(os.environ["ADK_TEST_BACKEND_LOG"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps({"backend": backend, "args": sys.argv[1:]}) + "\n")
if backend == "curl":
    program = os.environ["ADK_TEST_REAL_CURL"]
    os.execv(program, [program, *sys.argv[1:]])
if backend == "wget":
    # A selected but failing backend must leave the existing target intact.
    sys.exit(77)
sys.exit(78)
