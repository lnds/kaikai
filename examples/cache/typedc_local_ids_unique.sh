#!/bin/sh
# A local binder's id is unique across the whole program, not only within its
# function: passes compare ids from different functions (the evidence count,
# the native raw-binder table, the refinement call-site comparison, fused
# tail groups). The record-field hoist names its temporaries from the binder
# id, so three functions with the same shape must carry three different ids.
# The ids also come from the declaration's place in the stream, so adding a
# declaration after them leaves them as they were.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() {
  echo "typedc_local_ids_unique: FAIL — $1"
  exit 1
}

mkdir -p "$WORK/a" "$WORK/b"
cat > "$WORK/a/main.kai" <<'EOF'
type Buf = { data: Vec[Int], len: Int }

fn put1(s: Buf, i: Int) : Buf = Buf { data: vec_set(s.data, i, 1), len: s.len + 1 }
fn put2(s: Buf, i: Int) : Buf = Buf { data: vec_set(s.data, i, 2), len: s.len + 1 }
fn put3(s: Buf, i: Int) : Buf = Buf { data: vec_set(s.data, i, 3), len: s.len + 1 }

fn main() : Unit / Stdout = {
  let s = put3(put2(put1(Buf { data: vec_make(4, 0), len: 0 }, 1), 2), 3)
  Stdout.print(int_to_string(vec_get(s.data, 1) + s.len))
}
EOF
cp "$WORK/a/main.kai" "$WORK/b/main.kai"
printf '\nfn extra_after(a: Int) : Int = {\n  let t = a + 1\n  t * 2\n}\n' >> "$WORK/b/main.kai"

for p in a b; do
  "$KAIC2" $EDITION_FLAG --emit=kir-noinline --path "$ROOT/stdlib" "$WORK/$p/main.kai" > "$WORK/$p.kir" 2> "$WORK/$p.err" || { cat "$WORK/$p.err"; fail "$p/main.kai does not compile"; }
  for f in put1 put2 put3; do
    awk -v f="fn kaiu_main__$f(" 'index($0, f) == 1 { on = 1 } on { print } on && /^}/ { exit }' "$WORK/$p.kir" \
      | grep -o '__rfh_[0-9]*' | sort -u | head -1 > "$WORK/$p.$f"
    [ -s "$WORK/$p.$f" ] || fail "no hoisted local in $f of $p/main.kai"
  done
done

n="$(cat "$WORK"/a.put1 "$WORK"/a.put2 "$WORK"/a.put3 | sort -u | wc -l | tr -d ' ')"
[ "$n" -eq 3 ] || fail "three functions share a binder id ($n distinct)"
for f in put1 put2 put3; do
  cmp -s "$WORK/a.$f" "$WORK/b.$f" || fail "$f changed its binder id when a declaration was added after it"
done
echo "typedc_local_ids_unique: OK — binder ids differ across functions and do not move when a declaration is added after them"
