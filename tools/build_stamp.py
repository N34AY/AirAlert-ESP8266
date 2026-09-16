"""Build-time firmware metadata: version (from the VERSION file) and timestamp.

Single source of truth for the version is the repo-root `VERSION` file, so a
bump is one line there — no editing platformio.ini or code. This pre: script
injects it as AIRALERT_VERSION on every dev/prod/mock build, alongside
AIRALERT_BUILD (the compile time; __DATE__/__TIME__ only refresh when the file
using them recompiles, so an incremental build showed a stale date).
"""
import os
import re
import time

Import("env")  # noqa: F821 - injected by PlatformIO/SCons

version_path = os.path.join(env["PROJECT_DIR"], "VERSION")  # noqa: F821
with open(version_path) as f:
    version = f.read().strip()

if not re.fullmatch(r"\d+\.\d+\.\d+", version):
    raise SystemExit(
        "VERSION must be a SemVer 'X.Y.Z' line; got %r in %s" % (version, version_path))

env.Append(CPPDEFINES=[  # noqa: F821
    ("AIRALERT_VERSION", env.StringifyMacro(version)),  # noqa: F821
    ("AIRALERT_BUILD", env.StringifyMacro(time.strftime("%b %e %Y %H:%M:%S"))),  # noqa: F821
])
