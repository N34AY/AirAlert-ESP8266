"""PlatformIO pre: script - regenerate src/web/WebAssets.h from web/ on every
build, so an edited page can never ship stale. Writes only on change, so an
unchanged UI does not force a recompile."""
import os
import sys

Import("env")  # noqa: F821 - injected by PlatformIO/SCons

sys.path.insert(0, os.path.join(env["PROJECT_DIR"], "tools"))  # noqa: F821
import build_web  # noqa: E402

target = build_web.ROOT / "src/web/WebAssets.h"
content = build_web.render()
if not target.exists() or target.read_text() != content:
    target.write_text(content)
    print("-> src/web/WebAssets.h regenerated")
