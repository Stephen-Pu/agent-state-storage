"""Pytest config — makes the in-tree ``kvcache_sglang`` importable
without installation."""

import pathlib
import sys

_pkg_root = pathlib.Path(__file__).resolve().parents[1]
if str(_pkg_root) not in sys.path:
    sys.path.insert(0, str(_pkg_root))
