#!/bin/sh
# examples/packages/private_type_scope_struct — a private type of one module does not change how another module reads a type of the same name.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
KAI="$DIR/../../../bin/kai"

cd "$DIR" && "$KAI" typecheck . >/dev/null
