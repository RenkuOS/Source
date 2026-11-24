#!/usr/bin/env python3
# Copyright 2026, Michael Spanier.
# Distributed under the terms of the MIT License.
# SPDX-License-Identifier: MIT
"""Build and run the tests of net80211/ieee80211_roam_logic.c on the build
host: smoothing and when to switch access points."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else here.parents[4])
net80211 = root / 'src/libs/compat/openbsd_wlan/net80211'

with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / 'roam_logic_test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-parameter', '-fsanitize=address,undefined',
        '-I', str(net80211), str(net80211 / 'ieee80211_roam_logic.c'),
        str(here / 'roam_logic_test.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
