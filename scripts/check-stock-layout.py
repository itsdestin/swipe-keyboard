#!/usr/bin/env python3
"""Exercise real stock InputPanel in an isolated offscreen process (never the desktop).

For installed-package checks, pass --runtime-root /usr/share/swipe-keyboard,
--qml-dir /usr/share/swipe-keyboard/qml and --model-data /usr/share/swipe-keyboard.
The test executable is deliberately independent of source/build directory paths.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_executable", type=Path)
    parser.add_argument("--runtime-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--qml-dir", type=Path)
    parser.add_argument("--model-data", type=Path)
    parser.add_argument("--stock-layouts", type=Path, default=Path("/usr/share/plasma/keyboard/layouts"))
    args = parser.parse_args()
    root = args.runtime_root.resolve()
    qml = (args.qml_dir or root / "qml").resolve()
    models = (args.model_data or (root if (root / "models").is_dir() else root / "assets")).resolve()
    try:
        spec = importlib.util.spec_from_file_location("stock_layout_integration", root / "integration/layout.py")
        module = importlib.util.module_from_spec(spec)
        # Do not write __pycache__ into an installed, potentially read-only runtime.
        sys.dont_write_bytecode = True
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix="swipekb-stock-layout-") as temporary:
            private = Path(temporary)
            for name in ("home", "data", "config", "cache", "runtime", "private"):
                (private / name).mkdir(mode=0o700)
            layouts = private / "layouts"
            # Missing stock files and unsupported markers are failures, never skips.
            module.build_overlay(args.stock_layouts.resolve(), layouts)
            env = os.environ.copy()
            for name in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
                env.pop(name, None)
            env.update({
                "HOME": str(private / "home"),
                "XDG_DATA_HOME": str(private / "data"),
                "XDG_CONFIG_HOME": str(private / "config"),
                "XDG_CACHE_HOME": str(private / "cache"),
                "XDG_RUNTIME_DIR": str(private / "runtime"),
                "QT_QPA_PLATFORM": "offscreen",
                "QT_IM_MODULE": "qtvirtualkeyboard",
                "QT_QUICK_BACKEND": "software",
                "QML_IMPORT_PATH": str(qml),
                "QML2_IMPORT_PATH": str(qml),
                "QT_VIRTUALKEYBOARD_LAYOUT_PATH": str(layouts),
                "SWIPEKB_MODEL_DATA": str(models),
                "SWIPEKB_DATA": str(private / "private"),
            })
            print(f"Stock layout: {args.stock_layouts}; QML: {qml}; models: {models}", flush=True)
            return subprocess.run([str(args.test_executable.resolve())], env=env, timeout=60, check=False).returncode
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"Stock InputPanel regression check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
