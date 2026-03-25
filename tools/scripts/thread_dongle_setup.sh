#!/usr/bin/env bash
# thread_dongle_setup.sh — DEPRECATED: now uses OTBR Docker
#
# This script forwards to otbr_setup.sh which runs the OpenThread Border Router
# in a Docker container instead of a bare ot-daemon process.

echo "NOTE: Migrated to OTBR Docker. Forwarding to otbr_setup.sh..."
exec "$(dirname "$0")/otbr_setup.sh" "$@"
