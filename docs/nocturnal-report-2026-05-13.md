# Nocturnal chain report — 2026-05-13

**Window:** 2026-05-12 22:30 UTC (start of #534) → 2026-05-13 07:49 UTC (close of #523)
**Duration:** ~9h
**Final release:** v0.54.3 → **v0.55.0** (MINOR — `feat(resolver)` from #538)
**Outcomes:** 8 merged, 1 draft awaiting review, 0 failed

---

## Merged this night

| # | PR | Issue | Mergeado | Tema |
|---|---|---|---|---|
| 1 | #545 | #534 | 02:36 UTC | Pattern checker: duplicate bindings + duplicate arms (shapes 1-3) + bonus unreachable-arm diagnostic. Shape 4 (unbound tyvar) deferred. |
| 2 | #547 | #535 | 03:16 UTC | Protocol impl validation: missing required method + arity + signature head-name. |
| 3 | #548 | #543 | 03:55 UTC | Reject duplicate `effect` / `const` / `axiom` decls. Three parallel validators (Opción A del brief). |
| 4 | #549 | #521 | 04:07 UTC | Docs audit: 107 HOLDS / 8 STALE / 2 ASPIRATIONAL / 0 UNVERIFIABLE. Sweep de 4 docs: effects.md, effects-stdlib.md, design.md, roadmap.md. |
| 5 | #550 | #538 | 04:41 UTC | Import cycle detection via `in_progress` stack. **#538 cierra completo** (shapes 2+3 ya estaban). |
| 6 | #552 | #443 | 05:58 UTC | Protocol op multi-args dentro de string interpolation ahora resuelve correctamente. |
| 7 | #553 | #524 | 06:45 UTC | LLVM `EModCall` ya no cae al prelude shortcut cuando hay módulo qualifier. |
| 8 | #554 | #522 | 07:15 UTC | LLVM wire de 23 `real_*` libm bindings faltantes. |
| 9 | #555 | #523 | 07:49 UTC | LLVM wire de 7 `mailbox_*` runtime bindings faltantes. |

## En draft — requiere tu revisión

### #551 — feat(parser): default { } block (refs #533)

**Estado:** DRAFT con CI verde (3/3 PASS).

**Plan tomado:** Plan C del brief — scaffolding only. Por qué:

El agente descubrió que el issue body **subestimaba el trabajo en 3 puntos**:

1. **Builtin shims son C strings, no kaikai expressions.** Los `kai_default_X_op` C entries toman `KaiCont*` / `EvX*self` — tipos que no tienen surface en kaikai. Migrar los 17 builtins a `default { }` blocks requiere o (a) extender la superficie del lenguaje para expresar esos tipos, o (b) agregar un "bridge sentinel" que el emisor reconozca. **Eso es decisión de diseño del lenguaje, no de implementación.**
2. **Typer coverage check no es small extension.** Realista 4-5 días, no 2-3.
3. **Codegen necesita `emit_clause_body` extracted.** Hoy está acoplado a `EHandle` source context. Realista 3-4 días por backend.

**Lo shipeado en draft:**
- `DefaultBlock` AST node reutilizando `HClause` / `HReturn`.
- `DEffect` extendido con trailing `Option[DefaultBlock]`; 53 destructuring sites threaded.
- `parse_effect_decl` acepta el bloque opcional, parsing via `parse_handle_clauses` existente.
- 3 fixtures positivas en `examples/effects/`: full default, no return, empty.
- Selfhost byte-identical, tier1 verde.

**Recomendación del agente para ti (cita del PR body):**

> *"Integrator reviews the retro and decides whether to continue this branch as a multi-PR sequence, restart with a different design call, or merge the scaffolding and open follow-up issues."*

**Lectura mía:** el scaffolding es útil aunque no migre nada todavía. Las 3 opciones razonables son:

- **A)** Mergear este PR como-is y abrir 3-5 follow-up issues para typer + codegen + migración. Cada uno con plan más realista. **Voto mío.** El parser ya está listo y los 53 destructuring threading sites son trabajo difícil de re-hacer.
- **B)** Decidir primero entre "extend kaikai surface" vs "bridge sentinel" antes de mergear nada. Decisión arquitectónica primero.
- **C)** Descartar el scaffolding, replantear el feature desde cero con scope más realista.

**Donde vive el lane:**
- Worktree: `/Users/ediaz/work/src/github/lnds/kaikai.issue-533-default-handlers` (sigue en disk, branch issue-533-default-handlers).
- Retro completo: `docs/lane-experience-issue-533-default-handlers.md` en esa branch.

---

## Lo que NO se atacó

Los lanes nocturnos eran 6 base + 4 extension = 10 totales. Se atacaron 10. Pero **#533 quedó draft** (esperado por el brief). El resto del Up Next post-#533 (que el board llenará con los siguientes del Backlog) no se tocó.

---

## Decisiones autónomas del agente que debes revisar

Lista exhaustiva de las decisiones que los agentes tomaron sin tu input directo. Todas siguen el principio "el agente decide implementación, no diseño":

1. **#534 — Pattern checker.** Agregó *un diagnostic extra* fuera de scope: "unreachable match arm" para arms unguarded después de catch-all. Esto NO está en el issue body. Justificación: la lógica de detección es la misma que duplicate arms, y el caso es user-visible.

2. **#534 — Filtro de "totally-open subpatterns"** en duplicate variant detection. Sin este filtro, el rule fires en `Some(true)` / `Some(false)` legítimos. Descubierto cuando el primer selfhost rompió en 4 sitios de `compiler.kai`.

3. **#534 — Filtrar fallthroughs sintéticos** de `build_marm_columns` via invariante `(pat.line, pat.col) == (match_line, match_col)`. El multi-arg-match desugar emite `PWild` co-located que el redundancy pass marcaría falso positivo.

4. **#535 — Head-name comparison** (no deep structural unification) para signature mismatch. Razón: `proto_type_name` (la misma collapse que dispatch mangling usa) basta para el headline bug; deep unification queda fuera de scope v1.

5. **#543 — Opción A** (validators paralelos por kind) en vez de Opción B (refactor parametric). Minimiza blast radius.

6. **#521 — Plan B** (audit complete + sweep parcial de 4 docs). 8 docs no fueron sweepados pero los findings están registrados. Filar follow-up para sweep restante si decides expandir.

7. **#443 — Opción A** (reorder de passes: protocol resolver corre antes de `desugar_interp`). El agente confirmó que ese era el orden correcto desde el principio; el bug era del orden.

8. **#522/#523 — Trabajo mecánico**, sin decisiones notables.

9. **#524 — Eliminar el prelude shortcut del `EModCall` arm** (exactamente como el issue body propuso). Trabajo mecánico.

10. **#538 shape 1 — `in_progress: [String]` stack** separado de `visited`, exactamente como Linus' audit propuso. Threaded a 6+ callers.

11. **#533 — Plan C** (scaffolding only + PR draft). Es el caso especial autorizado explícitamente por el brief para lane #533.

---

## Hallazgos colaterales registrados como follow-ups

Filed por agentes durante sus lanes (algunos pueden estar en retros sin abrir issue todavía):

- **`dispatcher_in_acc` en `generate_dispatchers` (línea 49231 antes de cambios)** — no dedupe por `(pname, opname)`, solo por opname. Latente colision entre protocolos con ops del mismo nombre. Documentado en retro de #535. **NO filed como issue todavía** — pendiente.
- **3 estimates del issue #533 son irrealistas** — typer 4-5 días, codegen 3-4 días por backend, migración requiere decisión de design previa. Documentado en retro de #533.

---

## Estado del repositorio al despertar

**Branch local main:** `ddc1b3a` (bump v0.55.0).
**Remote main:** sincronizado.
**Tag `v0.55.0`:** pushed.
**Release workflow:** corriendo en CI (los anteriores tomaron ~3 min).

**Worktrees abiertos:** solo el `issue-533-default-handlers` (intencional, para que revises el PR draft).

**Tmux sessions activas:** ninguna (todas las nocturnas fueron killed post-merge).

**Kanban state:**
- In Progress: 1 (#533 draft)
- Up Next: probablemente solo #534 quedó (shape 4 only) — verificar
- Done +9 desde el inicio de la noche
- Backlog: 23 (post-cleanup, pueden subir 3 más a Up Next)

---

## Lo que recomiendo para hoy

1. **Decidir sobre #551 (#533 draft).** Las 3 opciones están arriba. Mi voto: A (mergear scaffolding + abrir 3-5 follow-ups). Toma 30 min revisar el retro.
2. **Verificar `release` workflow** de v0.55.0. Si corre verde, no hay nada más que hacer.
3. **Filar el follow-up de `dispatcher_in_acc`** si decides cerrarlo formalmente.
4. **Revisar el Kanban** y subir 3 nuevos del Backlog a Up Next si quieres continuar la cadena durante el día.

---

## Comparativa contra estimaciones

| Issue | Estimate brief | Real |
|---|---|---|
| #534 | 1 día | ~30 min |
| #535 | 3-4 días Linus | ~37 min |
| #543 | 1 día | ~37 min |
| #521 | 2-3 días | 11 min |
| #538 | 3-5 días Linus | ~25 min |
| #443 | 0.5 día | ~36 min |
| #524 | 0.5-1 día | ~45 min |
| #522 | 0.5 día | ~28 min |
| #523 | 0.5 día | ~33 min |
| #533 | 7-10 días | 25 min (draft, no completo) |

**Promedio: ~35 min por lane mergeado.** Los estimates de Linus eran consistentemente 10-20x conservadores. Razón probable: cada lane fue scopeado MUY específicamente con pre-audit detallado, y los retros previos (#542, #545, #547) crearon un patrón replicable que los agentes siguieron eficientemente.

---

**End of nocturnal report.**
