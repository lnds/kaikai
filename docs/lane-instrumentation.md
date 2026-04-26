# Lane Instrumentation Template

Reusable instrumentation snippet to insert into the prompt of every
agent lane, starting with Fase A (m12.8, m12.5, m5.x) and onwards.

Captures objective metrics that survive context truncation and agent
self-report bias: wall-clock from shell timestamps, build/test
invocations from a TSV log written by the agent at each `make` call.

Feeds into the LLM authorability baseline tracked in
`docs/llm-authorship-baseline.md`.

## How to use

1. When preparing the prompt for a new lane, copy the snippet block
   below verbatim into the prompt body, between "## Plan de
   implementación" and "## Hard rules".
2. Replace the placeholder `<nombre-del-lane>` with the actual lane
   identifier (e.g. `m12.8`, `m12.5`, `m5x-1-2`, `m12.6-refinements`).
3. The agent executes shell commands as part of normal work; the log
   is written to `/tmp/lane-${LANE}-builds.tsv` and committed at the
   end as `docs/lane-experience-${LANE}.md`.

## Snippet content

The block below is in Spanish (consistent with the rest of the agent
prompts in this project). The agent's output report (`docs/lane-
experience-*.md`) must be in English (project documentation rule).

```markdown
## Instrumentation (obligatorio para baseline empírico)

Este lane forma parte de la captura de baseline empírico para la
hipótesis Tier 3 de LLM authorability. Sigue estos pasos en orden.

### Al empezar el lane (primer paso, antes de leer código)

Define `LANE` con el nombre exacto de tu lane (ej. `m12.8`, `m12.5`,
`m5x-1-2`) y registra timestamp de inicio:

```bash
export LANE="<nombre-del-lane>"   # rellenar con tu lane específico
date -Iseconds > /tmp/lane-${LANE}-start.txt
echo -e "timestamp\tcmd\toutcome\telapsed_s" > /tmp/lane-${LANE}-builds.tsv
```

### Cada vez que corras `make all`, `make test`, o `make selfhost`

Inmediatamente después del comando (sin importar si OK o FAIL), anota
en el log de builds. Forma corta:

```bash
make test && echo -e "$(date -Iseconds)\ttest\tOK\t-" >> /tmp/lane-${LANE}-builds.tsv \
          || echo -e "$(date -Iseconds)\ttest\tFAIL\t-" >> /tmp/lane-${LANE}-builds.tsv
```

(Adaptar `test` a `all` o `selfhost` según el comando. El `-` en
`elapsed_s` queda vacío en Nivel 1.)

### Al cerrar el lane (último paso, antes de reportar al user)

1. Registra timestamp de cierre:

   ```bash
   date -Iseconds > /tmp/lane-${LANE}-end.txt
   ```

2. Escribe `docs/lane-experience-${LANE}.md` (en INGLÉS, regla del
   proyecto) con el formato siguiente:

   ```markdown
   # Lane experience report — ${LANE}

   Best-effort retrospective by the implementing agent. See limitations
   at the bottom.

   ## Objective metrics (from /tmp/lane-${LANE}-builds.tsv)

   - Start: <contenido de /tmp/lane-${LANE}-start.txt>
   - End:   <contenido de /tmp/lane-${LANE}-end.txt>
   - Wall-clock: <end - start, calculated>
   - Build/test invocations:
     - `make all`:      N invocations, M passes, K fails
     - `make test`:     N invocations, M passes, K fails
     - `make selfhost`: N invocations, M passes, K fails

   ## Compiler errors I encountered

   For each distinct error class still in your context:
   1. **[error type / message snippet]** — at [parser / check / emit / runtime] — fixed by [what change]. Took ~N attempts.
   2. ...

   If none visible: "no compiler errors visible in current context".

   ## Friction points

   Free-form. Sections that took disproportionate effort, or where the
   spec / existing code surprised you. Be specific about the subsystem
   (parser / chk_expr / emit / etc.), what was hard, what helped you
   converge.

   ## Spec ambiguities or interpretive choices

   Anywhere the spec was open to interpretation and you had to decide.
   Examples: "Spec said X. I implemented Y. Reasoning: ...".
   If none: say so.

   ## Subjective summary

   - Confidence in correctness: [high / medium / low] because [...]
   - Hardest sub-task: [...]
   - Easiest sub-task: [...]
   - Did the compiler help or hinder you? Concrete examples.

   ## Limitations of this report

   - Self-report bias acknowledged.
   - Context truncation: counts and error lists exclude anything that
     fell out of my visible context window.
   - Single agent (Claude). Not generalisable across LLMs.
   ```

3. Append the build TSV to the report (raw data, never lies):

   ```bash
   echo "" >> docs/lane-experience-${LANE}.md
   echo "## Raw build log" >> docs/lane-experience-${LANE}.md
   echo "" >> docs/lane-experience-${LANE}.md
   echo '```' >> docs/lane-experience-${LANE}.md
   cat /tmp/lane-${LANE}-builds.tsv >> docs/lane-experience-${LANE}.md
   echo '```' >> docs/lane-experience-${LANE}.md
   ```

4. Commit en tu rama (NO mergear, no tocar main):

   ```
   git add docs/lane-experience-${LANE}.md
   git commit -m "docs: ${LANE} lane experience report (LLM baseline data)"
   ```

5. Limpia los archivos temporales:

   ```bash
   rm /tmp/lane-${LANE}-start.txt /tmp/lane-${LANE}-end.txt /tmp/lane-${LANE}-builds.tsv
   ```

### Importante

- NO modifiques `docs/llm-authorship-baseline.md` (es snapshot histórico
  cerrado).
- NO mergees el reporte a main; queda en tu rama hasta que el user
  decida.
- NO inventes números si el log está incompleto. Reporta honestamente
  "log truncated at iteration N" o "early attempts not captured".
```

## Notes for the prompt author

- Keep the snippet **literal** — agents have followed previous prompts
  faithfully when instructions are concrete shell commands.
- The TSV format (`timestamp\tcmd\toutcome\telapsed_s`) is stable; do
  not vary it across lanes or aggregation later breaks.
- The `<nombre-del-lane>` placeholder is the only field to substitute.
- For lanes with sub-tasks (e.g. m5.x has items 1 and 2), use a single
  `LANE` identifier covering the whole lane (`m5x-1-2`); the agent
  decides whether to commit per sub-task or once at the end, but the
  TSV stays in one file.

## Future levels (not yet implemented)

- **Nivel 2**: wrapper for `make` that captures `time` for each
  invocation. Requires touching the Makefile or instructing the agent
  to alias `make`. Adds the `elapsed_s` column with real values.
- **Nivel 3**: pre/post-hook that dumps `git diff --shortstat` into the
  TSV for each build, so the report can correlate "lines added" with
  "build outcome" (was the build broken because of a large refactor or
  a typo?).
- **Nivel 4**: full session transcript export from the agent's client.
  Out of scope for the agent itself; depends on the user's session
  configuration.
