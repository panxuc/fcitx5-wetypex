#!/bin/sh
set -eu
prefix=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export QTWEBENGINEPROCESS_PATH=/app/bin/QtWebEngineProcess
export QTWEBENGINE_RESOURCES_PATH=/app/resources
export QTWEBENGINE_LOCALES_PATH=/app/translations/qtwebengine_locales
export QTWEBENGINE_DICTIONARIES_PATH=/app/qtwebengine_dictionaries
exec "$prefix/libexec/fcitx5-wetypex-ai-viewer" "$@"
