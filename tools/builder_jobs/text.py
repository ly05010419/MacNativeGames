"""The language of what the jobs say to a player: Chinese when MNG_LANG starts with zh (the window sets it from the
Mac's first language), else English. Read at each call, so a test can switch it."""
import os


def t(en, zh):
    return zh if os.environ.get('MNG_LANG', '').startswith('zh') else en
