#!/bin/sh
# Regenerates plugin.zip and plugin.zip.sig here from sdk/template with the test-only key
# (TEST-ONLY-key.json, public half in test-public-key.txt; never use it for a real plugin).
# Needed only if the template or the packer changes; the key stays the same.
set -eu
cd "$(dirname "$0")/../../.."
node sdk/zk-plugin.mjs pack sdk/template
node sdk/zk-plugin.mjs sign sdk/template --key tests/data/catalog/TEST-ONLY-key.json
cp sdk/template/dist/plugin.zip sdk/template/dist/plugin.zip.sig tests/data/catalog/
