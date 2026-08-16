#!/bin/sh
# Runs once when the dev container is created (devcontainer.json
# postCreateCommand), from the repo root.
set -e
WS=$(pwd)

# Configure the host build so the C/C++ extension has compile_commands.json.
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

# Interactive shells see the machine address/token from the git-ignored
# .env (GF_HOST / GF_TOKEN), so the bench tools and curl work from a
# terminal without re-typing them. CRs are stripped in case the file was
# written by a Windows editor.
if ! grep -q 'forgectrl: expose GF_HOST' ~/.bashrc 2>/dev/null; then
    printf '%s\n' '' \
        '# forgectrl: expose GF_HOST / GF_TOKEN from the repo .env to shells' \
        "if [ -f '$WS/.env' ]; then set -a; eval \"\$(tr -d '\\r' < '$WS/.env')\"; set +a; fi" \
        >> ~/.bashrc
fi
