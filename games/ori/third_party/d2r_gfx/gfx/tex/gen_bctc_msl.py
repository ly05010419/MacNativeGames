#!/usr/bin/env python3
"""Wrap nvn_bctc.metal into nvn_bctc_msl.h (a C string the runtime compiles with newLibraryWithSource).
usage: python3 gen_bctc_msl.py   (run after editing nvn_bctc.metal; the Makefile does not run it)"""
from pathlib import Path
here = Path(__file__).resolve().parent
src = (here / 'nvn_bctc.metal').read_text()
lines = [l.replace('\\', '\\\\').replace('"', '\\"') for l in src.splitlines()]
out = ['// Generated from nvn_bctc.metal by gen_bctc_msl.py -- edit the .metal file, then rerun the script.', '#pragma once',
       'static const char k_bctc_msl[] =']
out += [f'"{l}\\n"' for l in lines]
out[-1] += ';'
(here / 'nvn_bctc_msl.h').write_text('\n'.join(out) + '\n')
