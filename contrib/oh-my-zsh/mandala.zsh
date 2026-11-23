#!/usr/bin/env zsh

# Thin launcher for the canonical mandalac renderer. The C executable owns the
# terminal lifecycle so this adapter cannot diverge into a second E8 animation.

emulate -L zsh
setopt localoptions no_aliases

local mandalac_bin=''
local project_bin=${0:A:h:h:h}/mandala
if [[ -n ${MANDALAC_BIN-} ]]; then
  mandalac_bin=$MANDALAC_BIN
elif [[ -x $project_bin ]]; then
  mandalac_bin=$project_bin
elif (( ${+commands[mandalac]} )); then
  mandalac_bin=${commands[mandalac]}
elif (( ${+commands[mandala]} )); then
  mandalac_bin=${commands[mandala]}
fi

if [[ -z $mandalac_bin || ! -x $mandalac_bin ]]; then
  print -u2 -r -- \
    'mandalac shellsaver: executable not found; install mandala/mandalac or set MANDALAC_BIN'
  exit 127
fi

exec "$mandalac_bin" --shellsaver "$@"
