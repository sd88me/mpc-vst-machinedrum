#!/bin/sh
# Undo the MPC OS patch (restores the original bytes; falls back to the full SD backup).
exec sh "$(dirname "$0")/install.sh" uninstall
