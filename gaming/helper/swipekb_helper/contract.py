"""The on-device half of the shared contract (gaming/contract/gaming-contract.json, 'device' block).
The daily canary reads the same file's 'checks'; this reads 'device'. One source of truth."""
import json
import re
from urllib.parse import urlsplit

_PATH = re.compile(r'^[A-Za-z_$][\w$]*(\.[A-Za-z_$][\w$]*)*$')


class ContractError(Exception):
    pass


class Device:
    def __init__(self, doc):
        try:
            d = doc['device']
            self.targets = {t['role']: t['title'] for t in d['targets']}
            self.keyboard_title = next(t['title'] for t in d['targets'] if t['role'] == 'keyboard page')
            self.client_title = next(t['title'] for t in d['targets'] if t['role'] == 'holds SteamClient')
            # WHY (review fix 1): a page's title is set by whoever wrote the page, and Steam's in-client web browser opens web
            # pages next to Steam's own. Only a page served from Steam's own UI address may be talked to or typed into.
            o = urlsplit(d['ui_origin']['origin'])
            if o.scheme != 'https' or not o.hostname or o.port or o.username or o.path not in ('', '/') or o.query or o.fragment:
                raise ContractError('bad ui_origin')
            self.ui_host = o.hostname
            self.methods = list(d['steam_client_methods'])
            for m in self.methods:
                if not (_PATH.match(m['object']) and _PATH.match(m['name']) and m['target'] == self.client_title):
                    raise ContractError('bad steam_client_methods entry')
            # WHY forwarded verbatim: the stub carries these to the page; the real layer applies them at each
            # keyboard open (the keys do not exist until the keyboard is on screen).
            self.page_config = {'keyboard_dom': d['keyboard_dom'], 'password_signal': d['password_signal']}
        except (KeyError, TypeError, StopIteration, ContractError) as e:
            raise ContractError('contract device block unusable: %s' % (e,))

    def is_steam_url(self, url):
        """True only for https://<Steam's UI host>/... exactly: no other host, no user:password@, no custom port, no http."""
        try:
            u = urlsplit(url)
            return u.scheme == 'https' and u.hostname == self.ui_host and u.port is None and u.username is None and u.password is None
        except ValueError:
            return False


def load(path):
    try:
        return Device(json.loads(path.read_text()))
    except (OSError, ValueError) as e:
        raise ContractError('contract unreadable: %s' % e)


def method_probe(m):
    """JavaScript run in the SteamClient page: 'ok', 'notready' (SteamClient not defined yet) or 'missing:<what>'."""
    return ("(function(){if(typeof SteamClient==='undefined')return 'notready';var o=window,p=%s,i;"
            "for(i=0;i<p.length;i++){o=o&&o[p[i]];if(!o)return 'missing:'+p.slice(0,i+1).join('.');}"
            "return typeof o[%s]==='function'?'ok':'missing:'+%s;})()"
            % (json.dumps(m['object'].split('.')), json.dumps(m['name']), json.dumps(m['object'] + '.' + m['name'])))
