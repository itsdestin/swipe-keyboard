"""Settings, and the one place that decides which network addresses the helper may touch.

WHY this is strict: the debug port gives full control of Steam's screens (plan Section 9). The helper must
only ever talk to Steam's own loopback port. SteamOS also forwards that port on 8081 to every network
interface; connecting there would be a pointless widening of exposure, so it is refused by name.
"""
import os
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urlsplit

HOST = '127.0.0.1'          # the only address this program connects to
DEFAULT_PORT = 8080         # Steam's debug port on loopback
FORBIDDEN_PORTS = (8081,)   # SteamOS's all-interfaces forward of the same port (plan Section 9, M0)


class ForbiddenEndpoint(ValueError):
    """Something tried to make the helper talk to an address or port it must never touch."""


def check_port(port):
    if port in FORBIDDEN_PORTS or not isinstance(port, int) or not (1 <= port <= 65535):
        raise ForbiddenEndpoint('port %r is not allowed' % (port,))
    return port


def check_ws_url(url, port):
    """Validate a websocket address and return (host, port, path). Only ws://127.0.0.1:<our port>/... passes."""
    u = urlsplit(url)
    if u.scheme != 'ws' or u.hostname != HOST or u.port != port:
        raise ForbiddenEndpoint('refusing %r' % url)
    return HOST, port, u.path


def _home():
    return Path(os.environ.get('HOME', '~')).expanduser()


def default_state_dir():
    # WHY: status lives under XDG state in /home, never in the read-only OS image (plan Section 6).
    base = os.environ.get('XDG_STATE_HOME') or str(_home() / '.local' / 'state')
    return Path(base) / 'swipekb-gaming'


def default_marker_dirs():
    # NOTE: the helper only uses these to fingerprint the Steam build; it never writes there and in its sandbox cannot see them.
    # WHY both: Steam reads the marker through either path; on SteamOS they are the same folder.
    return [_home() / '.steam' / 'steam', _home() / '.local' / 'share' / 'Steam']


def default_contract_path():
    # Next to the package in a source checkout; the Flatpak build will put the contract beside the helper.
    here = Path(__file__).resolve()
    cands = [here.parent / 'gaming-contract.json', here.parents[2] / 'contract' / 'gaming-contract.json']
    if len(here.parents) > 3:
        # the GamingHelper Flatpak: /app/share/swipekb-helper/gaming-contract.json (build-helper.sh)
        cands.append(here.parents[3] / 'share' / 'swipekb-helper' / 'gaming-contract.json')
    for cand in cands:
        if cand.exists():
            return cand
    return here.parent / 'gaming-contract.json'


def default_layer_dir():
    # WHY several places: a source checkout has gaming/layer next to gaming/helper; the GamingHelper Flatpak puts it
    # at /app/share/swipekb-helper/layer while this package sits in /app/lib/swipekb-helper (packaging/flatpak/build-helper.sh).
    here = Path(__file__).resolve()
    cands = (here.parents[2] / 'layer', here.parents[3] / 'share' / 'swipekb-helper' / 'layer' if len(here.parents) > 3 else here.parent / 'layer')
    for cand in cands:
        if (cand / 'gesture.js').is_file():
            return cand
    return cands[0]


def default_engine_paths():
    # WHY XDG_RUNTIME_DIR/swipekb: the same folder the engine daemon makes (gaming/daemon/main.cpp); a folder only our user can open.
    # WHY no /tmp fallback (review fix 7): /tmp is shared, so another user could make /tmp/swipekb first and hand us a socket and a
    # token. systemd's per-user folder is the same place XDG_RUNTIME_DIR would have named, and it is private to this user.
    run = os.environ.get('XDG_RUNTIME_DIR') or '/run/user/%d' % os.getuid()
    return Path(run) / 'swipekb' / 'engine.sock', Path(run) / 'swipekb' / 'engine.token'


@dataclass
class Config:
    port: int = DEFAULT_PORT
    state_dir: Path = field(default_factory=default_state_dir)
    marker_dirs: list = field(default_factory=default_marker_dirs)
    contract_path: Path = field(default_factory=default_contract_path)
    layer_dir: Path = field(default_factory=default_layer_dir)
    gesture: dict = field(default_factory=dict)   # hidden tuning knobs for the arbiter: swipe_px, hold_ms, stuck_ms (plan Section 7)
    engine_socket: Path = field(default_factory=lambda: default_engine_paths()[0])
    engine_token: Path = field(default_factory=lambda: default_engine_paths()[1])
    session: dict = field(default_factory=dict)   # hidden knobs for the typing session in the page: tick_ms, trail_ms
    failsafe: dict = field(default_factory=dict)  # hidden knobs for the fail-safe watcher in the page: tick_ms, limit, focus_ms
    poll_interval: float = 0.5        # port-closed probe and event-loop tick; a connect to loopback costs ~nothing
    beat_interval: float = 1.0        # liveness ping to the stub; also the safety net that notices a lost stub
    layer_watchdog: float = 6.0       # stub removes itself if no ping arrives for this long (helper crashed)
    call_timeout: float = 2.0         # any single request to Steam
    not_ready_grace: float = 15.0     # how long a page may lack SteamClient before that counts as "Steam changed"
    failure_spacing: float = 2.0      # failed contract checks closer together than this count once
    recheck_interval: float = 10.0    # while the layer is installed, how often the helper re-asks Steam's hidden page for its text call
    healthy_after: float = 30.0       # a layer must stay installed and answering this long before its past failures are forgiven
    max_backoff: float = 60.0         # longest wait between two tries against the same Steam build
    max_failures: int = 3             # per Steam build; then the helper stops trying (plan Section 14, M9)
    steamui_probe: object = 'auto'    # file whose mtime joins the "which Steam build" fingerprint; None = off

    def __post_init__(self):
        check_port(self.port)
        self.state_dir = Path(self.state_dir)
        self.contract_path = Path(self.contract_path)
        self.layer_dir = Path(self.layer_dir)
        self.engine_socket = Path(self.engine_socket)
        self.engine_token = Path(self.engine_token)
        self.marker_dirs = [Path(p) for p in self.marker_dirs]
