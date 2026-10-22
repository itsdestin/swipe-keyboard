"""swipekb-helper entry point:  python3 -m swipekb_helper   (run with the folder holding swipekb_helper on PYTHONPATH)."""
import os
import signal
import sys

from . import config, exposure
from .helper import Helper


def main():
    cfg = config.Config(port=int(os.environ.get('SWIPEKB_DEBUG_PORT', config.DEFAULT_PORT)),
                        **({'state_dir': os.environ['SWIPEKB_STATE_DIR']} if 'SWIPEKB_STATE_DIR' in os.environ else {}),
                        **({'contract_path': os.environ['SWIPEKB_CONTRACT']} if 'SWIPEKB_CONTRACT' in os.environ else {}),
                        # WHY: tests and the VM rig point the helper at an engine somewhere else; real installs use the default folder
                        **({'engine_socket': os.environ['SWIPEKB_ENGINE_SOCKET']} if 'SWIPEKB_ENGINE_SOCKET' in os.environ else {}),
                        **({'engine_token': os.environ['SWIPEKB_ENGINE_TOKEN']} if 'SWIPEKB_ENGINE_TOKEN' in os.environ else {}))
    # WHY the check is on here: the installed helper keeps the layer off while the debug port is open to the network.
    h = Helper(cfg, exposure_check=exposure.open_doors)

    def stop(*_):
        # WHY: systemd stops us with SIGTERM; take the layer out of Steam before leaving.
        h._stop.set()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    h.run()      # runs in this thread until stopped, then cleans up and writes "off"
    return 0


if __name__ == '__main__':
    sys.exit(main())
