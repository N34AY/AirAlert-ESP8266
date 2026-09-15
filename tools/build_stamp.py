"""Per-build timestamp for the Web UI "build" field.

__DATE__/__TIME__ only refresh when the file that uses them is recompiled,
so an incremental build showed a stale date. This runs on every `pio run`.
"""
import time

Import("env")  # noqa: F821 - injected by PlatformIO/SCons

env.Append(CPPDEFINES=[("AIRALERT_BUILD",
                        env.StringifyMacro(time.strftime("%b %e %Y %H:%M:%S")))])
