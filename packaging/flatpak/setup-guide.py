#!/usr/bin/env python3
"""Explain the separate bundled keyboard before the user selects it in KDE."""
import sys


def main():
    # WHY: the sandbox deliberately cannot inspect/write host keyboard settings.
    # Consent explains the change; KDE Settings remains the explicit selection boundary.
    print('Swipe Keyboard for SteamOS Desktop Mode\n\n'
          'This Flatpak contains a separate bundled Plasma Keyboard. It does not replace '
          'SteamOS libraries, remove Maliit, or change Steam\'s Gaming Mode keyboard.\n'
          'Installing the bundle does not activate it or change your selected input method.\n\n'
          'You may notice a different keyboard appearance, layout/settings, suggestions and '
          'Backspace behavior after selecting it. Its language settings are separate from '
          'your previous keyboard. Contextual predictions are English-only.\n'
          'Desktop Mode only; Gaming Mode is not supported.\n'
          'Remember your previous selection so you can restore it before uninstalling.\n')
    if not sys.stdin.isatty():
        print('Open this guide in an interactive terminal to continue. No settings changed.')
        return 0
    try:
        answer = input('Continue to the manual setup instructions? [y/N] ').strip().lower()
    except (EOFError, KeyboardInterrupt):
        answer = ''
    if answer not in ('y', 'yes'):
        print('Cancelled. No settings changed.')
        return 0
    print('\nNext steps:\n'
          '1. Open System Settings → Virtual Keyboard and note your previous selection (including None).\n'
          '2. Select Swipe Keyboard and apply. This changes the Desktop Mode input method.\n'
          '3. Enable languages with:\n'
          '   flatpak run --user --command=kcmshell6 org.itsdestin.SwipeKeyboard kcm_plasmakeyboard\n'
          '   Maximize the settings window if the language list is clipped.\n'
          '4. Touch an ordinary text field to try the keyboard.\n'
          '5. Restore your previous selection in KDE Settings before uninstalling.\n\n'
          'This guide has made no changes. No settings changed automatically.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
