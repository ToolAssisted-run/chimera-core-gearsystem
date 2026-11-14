#!/usr/bin/env python3
# Writes the window's settings for a frontend run: empty but for the firmware
# given, remembered as the Firmware window remembers a file chosen by hand
# (keyed under this core's name). The run's machine settings are its
# project's, not the settings file's: a game runs only in a project (Chimera,
# 2026-10-26), and run-frontend.sh makes one with them.
#
# Usage: settings-config.py <output settings> [<firmware JSON>]
# The optional firmware map is {"<decl id>": "<file path>"}.
import json
import sys

out = {}
if len(sys.argv) > 2:
    # kept by the window as JSON text, as all its settings that hold JSON are
    out["cores.chosenFirmware"] = json.dumps({"Gearsystem": json.loads(sys.argv[2])})
json.dump(out, open(sys.argv[1], "w"), indent=2)
