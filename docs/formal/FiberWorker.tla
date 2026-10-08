---------------------------- MODULE FiberWorker ----------------------------
(***************************************************************************)
(* M:N fibers on work-stealing workers, and the scheduler state a fiber    *)
(* reaches after a switch. Written against 7799bf9b.                       *)
(*                                                                         *)
(* Mirrors stage2/runtime.h: kai_worker_loop, kai_worker_find_work,        *)
(* kai_sched_dequeue, kai_sched_steal_from, kai_sched_enqueue,             *)
(* kai_sched_yield, kai_sched_park, kai_sched_commit_park,                 *)
(* kai_drain_requeue_stack, kai_sched_remote_unpark, kai_fiber_pinned_to,  *)
(* kai_mailbox_pop, kai_mailbox_push_cross_thread, kai_default_spawn_await,*)
(* the trampoline's terminate walk, and the segment entry points           *)
(* kai_seg_start, kai_seg_suspend, kai_seg_resume, kai_seg_cont_drop,      *)
(* kai_seg_escape, kai_seg_rethrow.                                        *)
(*                                                                         *)
(* Ablate removes one fence, to check the search can see it fail: "none",  *)
(* "update" (no worker store on dispatch), "pin" (thieves ignore seg_pin), *)
(* "permit" (commit ignores wake_pending), "dropheld" (a discontinue       *)
(* leaves the boxes its frames own).                                       *)
(*                                                                         *)
(* Mode = "tls":   after a switch a function touches the worker whose     *)
(*                 thread pointer it read before the switch, or re-reads  *)
(*                 it; the compiler is free to do either.                  *)
(* Mode = "fiber": it touches worker[self], which the scheduler stores on  *)
(*                 every dispatch. `self` is the same fiber on both sides  *)
(*                 of the switch, so caching it is harmless; the field is  *)
(*                 reloaded because the switch is an opaque call and the   *)
(*                 fiber is reachable from the deques.                     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Workers, R, S1, S2, G, Segs, Mode, WithGen, Budget, Ablate

Root  == "root"
None  == "none"
Stack == "stack"
Pad   == "pad"

\* R receives two messages; S1 sends m1, yields, exits; S2 sends m2, awaits
\* S1, exits; G drives stack segments and is the Cancel target.
Fibers == IF WithGen THEN {R, S1, S2, G} ELSE {R, S1, S2}
Spawned == IF WithGen THEN <<R, S1, S2, G>> ELSE <<R, S1, S2>>
Msgs == {"m1", "m2"}
MsgOf(f) == IF f = S1 THEN "m1" ELSE "m2"
SenderOf(m) == IF m = "m1" THEN S1 ELSE S2
W1 == CHOOSE w \in Workers : TRUE
NSegs == Cardinality(Segs)

VARIABLES
    running,      \* worker -> the fiber on it, or Root (its scheduler loop)
    drainF,       \* worker -> fiber its root must publish after the exit swap
    drainK,       \* worker -> "park" | "yield"
    deque,        \* worker -> steal list (FIFO, owner and thieves take the head)
    state,        \* fiber -> "ready" | "running" | "parked" | "done"
    home,         \* fiber -> home_thread
    worker,       \* fiber -> the worker pointer the fix adds
    wakePending,  \* fiber -> wake_pending permits
    pc, after,    \* fiber -> program label; label to continue at after a resume
    cache,        \* fiber -> worker whose thread pointer it read before its switch
    active,       \* worker -> kai_active_fiber of that worker
    misTouch,     \* a fiber touched scheduler state of a worker it was not on
    mailbox, recvd, waiters, awaiters, wakeSet,
    pinned, pinW, \* seg_pin, and the worker it was taken on
    ctx,          \* G's running context: Stack or a segment
    segState,     \* segment -> "none" | "running" | "suspended" | "dead"
    parent,       \* segment -> the context it hangs from while on the chain
    holder,       \* segment -> context whose frame holds its Cont box, or None
    cleanup,      \* segment -> times its `finally` chain ran
    budget, cancelReq, cancelSeen, escTo, dropping

sched == <<running, drainF, drainK, deque, state, home, worker, wakePending,
           pc, after, cache, active, misTouch>>
mbox  == <<mailbox, recvd, waiters, awaiters, wakeSet>>
segv  == <<pinned, pinW, ctx, segState, parent, holder, cleanup, budget,
           cancelReq, cancelSeen, escTo, dropping>>
vars  == <<sched, mbox, segv>>

Init ==
    /\ running = [w \in Workers |-> Root]
    /\ drainF = [w \in Workers |-> None]
    /\ drainK = [w \in Workers |-> None]
    /\ deque = [w \in Workers |-> IF w = W1 THEN Spawned ELSE <<>>]
    /\ state = [f \in Fibers |-> "ready"]
    /\ home = [f \in Fibers |-> W1]
    /\ worker = [f \in Fibers |-> W1]
    /\ wakePending = [f \in Fibers |-> 0]
    /\ pc = [f \in Fibers |-> CASE f = R -> "r_lock" [] f = G -> "g_act"
                                   [] OTHER -> "s_send"]
    /\ after = [f \in Fibers |-> None]
    /\ cache = [f \in Fibers |-> W1]
    /\ active = [w \in Workers |-> Root]
    /\ misTouch = FALSE
    /\ mailbox = <<>>
    /\ recvd = <<>>
    /\ waiters = {}
    /\ awaiters = [f \in Fibers |-> {}]
    /\ wakeSet = [f \in Fibers |-> {}]
    /\ pinned = [f \in Fibers |-> FALSE]
    /\ pinW = W1
    /\ ctx = Stack
    /\ segState = [s \in Segs |-> "none"]
    /\ parent = [s \in Segs |-> Stack]
    /\ holder = [s \in Segs |-> None]
    /\ cleanup = [s \in Segs |-> 0]
    /\ budget = Budget
    /\ cancelReq = FALSE
    /\ cancelSeen = FALSE
    /\ escTo = None
    /\ dropping = None

IsRunning(f) == \E w \in Workers : running[w] = f
OnW(f) == CHOOSE w \in Workers : running[w] = f

-----------------------------------------------------------------------------
(* Scheduler *)

\* kai_sched_remote_unpark, under the target's current home slot lock.
Unpark(t) ==
    \/ /\ state[t] = "parked"
       /\ state' = [state EXCEPT ![t] = "ready"]
       /\ deque' = [deque EXCEPT ![home[t]] = Append(@, t)]
       /\ UNCHANGED wakePending
    \/ /\ state[t] = "done"
       /\ UNCHANGED <<state, deque, wakePending>>
    \/ /\ state[t] \in {"running", "ready"}
       /\ wakePending' = [wakePending EXCEPT ![t] = @ + 1]
       /\ UNCHANGED <<state, deque>>

\* The swap to the scheduler root. State stays "running" until the root
\* publishes the fiber, so a thief never sees a half-saved context.
Switch(f, kind, next) ==
    LET w == OnW(f) IN
    /\ running' = [running EXCEPT ![w] = Root]
    /\ active' = [active EXCEPT ![w] = Root]
    /\ drainF' = [drainF EXCEPT ![w] = f]
    /\ drainK' = [drainK EXCEPT ![w] = kind]
    /\ cache' = [cache EXCEPT ![f] = w]
    /\ after' = [after EXCEPT ![f] = next]
    /\ pc' = [pc EXCEPT ![f] = "use"]

\* kai_drain_requeue_stack / kai_drain_commit_stack on the root of w.
WDrain(w) ==
    LET f == drainF[w] IN
    /\ f # None
    /\ drainF' = [drainF EXCEPT ![w] = None]
    /\ drainK' = [drainK EXCEPT ![w] = None]
    /\ IF drainK[w] = "yield"
       THEN LET o == IF pinned[f] THEN home[f] ELSE w IN
            /\ state' = [state EXCEPT ![f] = "ready"]
            /\ deque' = [deque EXCEPT ![o] = Append(@, f)]
            /\ home' = [home EXCEPT ![f] = o]
            /\ UNCHANGED wakePending
       ELSE IF wakePending[f] > 0 /\ Ablate # "permit"
       THEN /\ wakePending' = [wakePending EXCEPT ![f] = @ - 1]
            /\ state' = [state EXCEPT ![f] = "ready"]
            /\ deque' = [deque EXCEPT ![home[f]] = Append(@, f)]
            /\ UNCHANGED home
       ELSE /\ state' = [state EXCEPT ![f] = "parked"]
            /\ UNCHANGED <<deque, home, wakePending>>
    /\ UNCHANGED <<running, worker, pc, after, cache, active, misTouch, mbox, segv>>

Run(w, f) ==
    /\ state' = [state EXCEPT ![f] = "running"]
    /\ running' = [running EXCEPT ![w] = f]
    /\ active' = [active EXCEPT ![w] = f]
    /\ worker' = IF Ablate = "update" THEN worker ELSE [worker EXCEPT ![f] = w]

\* kai_worker_find_work: own head first, else steal an unpinned head.
WDispatch(w) ==
    /\ running[w] = Root
    /\ drainF[w] = None
    /\ \/ /\ deque[w] # <<>>
          /\ deque' = [deque EXCEPT ![w] = Tail(@)]
          /\ Run(w, Head(deque[w]))
          /\ UNCHANGED home
       \/ /\ deque[w] = <<>>
          /\ \E v \in Workers \ {w} :
                /\ deque[v] # <<>>
                /\ (~pinned[Head(deque[v])] \/ Ablate = "pin")
                /\ deque' = [deque EXCEPT ![v] = Tail(@)]
                /\ home' = [home EXCEPT ![Head(deque[v])] = w]
                /\ Run(w, Head(deque[v]))
    /\ UNCHANGED <<drainF, drainK, wakePending, pc, after, cache, misTouch, mbox, segv>>

-----------------------------------------------------------------------------
(* The first thing a resumed fiber does: touch "its" worker's scheduler     *)
(* state (kai_set_active_fiber, kai_drain_pending_free, the commit and      *)
(* requeue stacks), and observe a pending Cancel.                           *)

Use(f) ==
    LET w == OnW(f)
        choices == IF Mode = "tls" THEN {cache[f], w} ELSE {worker[f]}
    IN
    /\ pc[f] = "use"
    /\ \E used \in choices :
          /\ active' = [active EXCEPT ![used] = f]
          /\ misTouch' = (misTouch \/ used # w)
    /\ IF f = G /\ cancelReq /\ ~cancelSeen /\ dropping = None
       THEN /\ pc' = [pc EXCEPT ![f] = "g_esc"]
            /\ cancelSeen' = TRUE
            /\ escTo' = Pad
       ELSE /\ pc' = [pc EXCEPT ![f] = after[f]]
            /\ UNCHANGED <<cancelSeen, escTo>>
    /\ UNCHANGED <<running, drainF, drainK, deque, state, home, worker,
                   wakePending, after, cache, mbox, pinned, pinW, ctx,
                   segState, parent, holder, cleanup, budget, cancelReq,
                   dropping>>

-----------------------------------------------------------------------------
(* Mailbox, await, termination *)

\* kai_mailbox_pop: one pass under the mailbox lock.
RLock ==
    /\ pc[R] = "r_lock"
    /\ IF mailbox # <<>>
       THEN /\ recvd' = Append(recvd, Head(mailbox))
            /\ mailbox' = Tail(mailbox)
            /\ waiters' = waiters \ {R}
            /\ pc' = [pc EXCEPT ![R] = IF Len(recvd) = 1 THEN "fin" ELSE "r_lock"]
       ELSE /\ waiters' = waiters \cup {R}
            /\ pc' = [pc EXCEPT ![R] = "r_park"]
            /\ UNCHANGED <<recvd, mailbox>>
    /\ UNCHANGED <<running, drainF, drainK, deque, state, home, worker,
                   wakePending, after, cache, active, misTouch, awaiters,
                   wakeSet, segv>>

RPark ==
    /\ pc[R] = "r_park"
    /\ Switch(R, "park", "r_lock")
    /\ UNCHANGED <<deque, state, home, worker, wakePending, misTouch, mbox, segv>>

\* kai_mailbox_push_cross_thread: append and dequeue a waiter under the
\* lock; the unpark runs after the unlock.
SSend(f) ==
    /\ pc[f] = "s_send"
    /\ mailbox' = Append(mailbox, MsgOf(f))
    /\ waiters' = {}
    /\ wakeSet' = [wakeSet EXCEPT ![f] = waiters]
    /\ pc' = [pc EXCEPT ![f] = "s_wake"]
    /\ UNCHANGED <<running, drainF, drainK, deque, state, home, worker,
                   wakePending, after, cache, active, misTouch, recvd,
                   awaiters, segv>>

\* Unpark what a send or a terminate walk dequeued, one target per step.
Wake(f, next) ==
    /\ IF wakeSet[f] # {}
       THEN \E t \in wakeSet[f] :
               /\ Unpark(t)
               /\ wakeSet' = [wakeSet EXCEPT ![f] = @ \ {t}]
               /\ UNCHANGED <<pc, running, active>>
       ELSE /\ next
            /\ UNCHANGED <<state, deque, wakePending, wakeSet>>
    /\ UNCHANGED <<drainF, drainK, home, worker, after, cache, misTouch,
                   mailbox, recvd, waiters, awaiters, segv>>

SWake(f) ==
    /\ pc[f] = "s_wake"
    /\ Wake(f, /\ pc' = [pc EXCEPT ![f] = IF f = S1 THEN "s_yield" ELSE "a_lock"]
               /\ UNCHANGED <<running, active>>)

SYield ==
    /\ pc[S1] = "s_yield"
    /\ Switch(S1, "yield", "fin")
    /\ UNCHANGED <<deque, state, home, worker, wakePending, misTouch, mbox, segv>>

\* kai_default_spawn_await: check-and-link under S1's slot lock.
ALock ==
    /\ pc[S2] = "a_lock"
    /\ IF state[S1] = "done"
       THEN /\ pc' = [pc EXCEPT ![S2] = "fin"]
            /\ UNCHANGED awaiters
       ELSE /\ awaiters' = [awaiters EXCEPT ![S1] = @ \cup {S2}]
            /\ pc' = [pc EXCEPT ![S2] = "a_park"]
    /\ UNCHANGED <<running, drainF, drainK, deque, state, home, worker,
                   wakePending, after, cache, active, misTouch, mailbox,
                   recvd, waiters, wakeSet, segv>>

APark ==
    /\ pc[S2] = "a_park"
    /\ Switch(S2, "park", "a_lock")
    /\ UNCHANGED <<deque, state, home, worker, wakePending, misTouch, mbox, segv>>

\* The trampoline's terminal store and awaiter snapshot, under its own slot lock.
Fin(f) ==
    /\ pc[f] = "fin"
    /\ state' = [state EXCEPT ![f] = "done"]
    /\ wakeSet' = [wakeSet EXCEPT ![f] = awaiters[f]]
    /\ awaiters' = [awaiters EXCEPT ![f] = {}]
    /\ pc' = [pc EXCEPT ![f] = "fin_wake"]
    /\ UNCHANGED <<running, drainF, drainK, deque, home, worker, wakePending,
                   after, cache, active, misTouch, mailbox, recvd, waiters, segv>>

FinWake(f) ==
    /\ pc[f] = "fin_wake"
    /\ Wake(f, LET w == OnW(f) IN
               /\ running' = [running EXCEPT ![w] = Root]
               /\ active' = [active EXCEPT ![w] = Root]
               /\ pc' = [pc EXCEPT ![f] = "exited"])

-----------------------------------------------------------------------------
(* Stack segments, on G *)

RECURSIVE ChainF(_, _)
ChainF(c, n) == IF c = Stack \/ n = 0 THEN {} ELSE {c} \cup ChainF(parent[c], n - 1)
Chain == ChainF(ctx, NSegs + 1)

RECURSIVE ReachesStack(_, _)
ReachesStack(c, n) ==
    IF c = Stack THEN TRUE ELSE IF n = 0 THEN FALSE ELSE ReachesStack(parent[c], n - 1)

RECURSIVE Closure(_, _)
Closure(S, n) == IF n = 0 THEN S ELSE Closure(S \cup {x \in Segs : holder[x] \in S}, n - 1)
\* Segments whose Cont boxes the frames of context c own, transitively.
HeldBy(c) == Closure({x \in Segs : holder[x] = c}, NSegs)

\* Unwind segments K: each runs its `finally` chain and goes back to a pool.
Kill(K) ==
    /\ segState' = [x \in Segs |-> IF x \in K THEN "dead" ELSE segState[x]]
    /\ cleanup' = [x \in Segs |-> IF x \in K THEN cleanup[x] + 1 ELSE cleanup[x]]
    /\ holder' = [x \in Segs |-> IF x \in K THEN None ELSE holder[x]]

GSched == <<running, drainF, drainK, deque, state, home, worker, wakePending,
            after, cache, active, misTouch>>

GStart(s) ==
    /\ budget > 0
    /\ segState[s] = "none"
    /\ segState' = [segState EXCEPT ![s] = "running"]
    /\ parent' = [parent EXCEPT ![s] = ctx]
    /\ ctx' = s
    /\ budget' = budget - 1
    /\ UNCHANGED <<pc, holder, cleanup, escTo, dropping, pinned, pinW>>

GSuspend ==
    /\ budget > 0
    /\ ctx \in Segs
    /\ segState' = [segState EXCEPT ![ctx] = "suspended"]
    /\ holder' = [holder EXCEPT ![ctx] = parent[ctx]]
    /\ ctx' = parent[ctx]
    /\ budget' = budget - 1
    /\ UNCHANGED <<pc, parent, cleanup, escTo, dropping, pinned, pinW>>

\* kai_seg_resume: empty the box and rebase the chain on the resumer.
GResume(s) ==
    /\ budget > 0
    /\ holder[s] = ctx
    /\ holder' = [holder EXCEPT ![s] = None]
    /\ parent' = [parent EXCEPT ![s] = ctx]
    /\ segState' = [segState EXCEPT ![s] = "running"]
    /\ ctx' = s
    /\ budget' = budget - 1
    /\ UNCHANGED <<pc, cleanup, escTo, dropping, pinned, pinW>>

GReturn ==
    /\ ctx \in Segs
    /\ HeldBy(ctx) = {}
    /\ Kill({ctx})
    /\ ctx' = parent[ctx]
    /\ UNCHANGED <<pc, parent, budget, escTo, dropping, pinned, pinW>>

\* kai_seg_cont_drop: the last ref to a box went away; discontinue its
\* segment on this worker, pinned.
GDrop(s) ==
    /\ holder[s] = ctx
    /\ holder' = [holder EXCEPT ![s] = None]
    /\ parent' = [parent EXCEPT ![s] = ctx]
    /\ segState' = [segState EXCEPT ![s] = "running"]
    /\ ctx' = s
    /\ dropping' = s
    /\ pinned' = [pinned EXCEPT ![G] = TRUE]
    /\ pinW' = OnW(G)
    /\ pc' = [pc EXCEPT ![G] = "g_drop"]
    /\ UNCHANGED <<cleanup, budget, escTo>>

\* A clause discards `resume` and jumps to a handle further out.
GAbandon(t) ==
    /\ budget > 0
    /\ ctx \in Segs
    /\ escTo' = t
    /\ budget' = budget - 1
    /\ pc' = [pc EXCEPT ![G] = "g_esc"]
    /\ UNCHANGED <<ctx, segState, parent, holder, cleanup, dropping, pinned, pinW>>

GDone ==
    /\ ctx = Stack
    /\ HeldBy(Stack) = {}
    /\ pc' = [pc EXCEPT ![G] = "fin"]
    /\ UNCHANGED <<ctx, segState, parent, holder, cleanup, budget, escTo,
                   dropping, pinned, pinW>>

GAct ==
    /\ pc[G] = "g_act"
    /\ \/ /\ \/ \E s \in Segs : GStart(s) \/ GResume(s) \/ GDrop(s)
             \/ GSuspend
             \/ GReturn
             \/ \E t \in ({Stack} \cup Chain) \ {ctx} : GAbandon(t)
             \/ GDone
          /\ UNCHANGED <<GSched, mbox, cancelReq, cancelSeen>>
       \/ /\ budget > 0
          /\ Switch(G, "yield", "g_act")
          /\ budget' = budget - 1
          /\ UNCHANGED <<deque, state, home, worker, wakePending, misTouch,
                         mbox, pinned, pinW, ctx, segState, parent, holder,
                         cleanup, cancelReq, cancelSeen, escTo, dropping>>

\* The discontinued segment unwinds; its `finally` may switch the fiber.
GDropStep ==
    /\ pc[G] = "g_drop"
    /\ \/ /\ budget > 0
          /\ Switch(G, "yield", "g_drop")
          /\ budget' = budget - 1
          /\ UNCHANGED <<deque, state, home, worker, wakePending, misTouch,
                         mbox, pinned, pinW, ctx, segState, parent, holder,
                         cleanup, cancelReq, cancelSeen, escTo, dropping>>
       \/ /\ Kill({dropping} \cup IF Ablate = "dropheld" THEN {} ELSE HeldBy(dropping))
          /\ ctx' = parent[dropping]
          /\ dropping' = None
          /\ pinned' = [pinned EXCEPT ![G] = FALSE]
          /\ pc' = [pc EXCEPT ![G] = "g_act"]
          /\ UNCHANGED <<GSched, mbox, pinW, parent, budget, cancelReq,
                         cancelSeen, escTo>>

\* kai_seg_escape + kai_seg_rethrow, one context per step: the running
\* segment unwinds to its entry, the resumer continues the exit.
GEsc ==
    /\ pc[G] = "g_esc"
    /\ IF ctx = escTo
       THEN /\ escTo' = None
            /\ pc' = [pc EXCEPT ![G] = "g_act"]
            /\ UNCHANGED <<ctx, segState, holder, cleanup>>
       ELSE IF ctx = Stack
       THEN /\ Kill(HeldBy(Stack))
            /\ escTo' = None
            /\ pc' = [pc EXCEPT ![G] = "fin"]
            /\ UNCHANGED ctx
       ELSE /\ Kill({ctx} \cup HeldBy(ctx))
            /\ ctx' = parent[ctx]
            /\ UNCHANGED <<pc, escTo>>
    /\ UNCHANGED <<GSched, mbox, pinned, pinW, parent, budget, cancelReq,
                   cancelSeen, dropping>>

\* Spawn.cancel(G) from anywhere: an atomic flag G reads at its yield points.
CancelG ==
    /\ WithGen
    /\ ~cancelReq
    /\ state[G] # "done"
    /\ cancelReq' = TRUE
    /\ UNCHANGED <<sched, mbox, pinned, pinW, ctx, segState, parent, holder,
                   cleanup, budget, cancelSeen, escTo, dropping>>

-----------------------------------------------------------------------------

FiberStep(f) ==
    /\ IsRunning(f)
    /\ \/ Use(f)
       \/ f = R /\ (RLock \/ RPark)
       \/ f \in {S1, S2} /\ (SSend(f) \/ SWake(f))
       \/ f = S1 /\ SYield
       \/ f = S2 /\ (ALock \/ APark)
       \/ f = G /\ (GAct \/ GDropStep \/ GEsc)
       \/ Fin(f)
       \/ FinWake(f)

WorkerStep(w) == WDispatch(w) \/ WDrain(w)

Finished == (\A f \in Fibers : pc[f] = "exited") /\ UNCHANGED vars

Next ==
    \/ \E w \in Workers : WorkerStep(w)
    \/ \E f \in Fibers : FiberStep(f)
    \/ CancelG
    \/ Finished

Spec == Init /\ [][Next]_vars
FairSpec ==
    /\ Spec
    /\ \A w \in Workers : WF_vars(WorkerStep(w))
    /\ \A f \in Fibers : WF_vars(FiberStep(f))

-----------------------------------------------------------------------------
(* Safety *)

Runs(f) == Cardinality({w \in Workers : running[w] = f})
Drains(f) == Cardinality({w \in Workers : drainF[w] = f})
Occ(f) == Cardinality({<<w, i>> \in Workers \X (1..Len(Spawned)) :
                          i <= Len(deque[w]) /\ deque[w][i] = f})
Count(seq, m) == Cardinality({i \in 1..Len(seq) : seq[i] = m})

\* A fiber running on W only touches W's scheduler state.
TouchesOwnWorker == ~misTouch
ActiveCoherent == \A w \in Workers : active[w] = running[w]

NoFiberOnTwoWorkers == \A f \in Fibers : Runs(f) <= 1

\* Every live fiber is in exactly one place; a parked one is reachable by a waker.
NoLostNoDouble ==
    \A f \in Fibers :
        CASE state[f] = "ready"   -> Occ(f) = 1 /\ Runs(f) + Drains(f) = 0
          [] state[f] = "running" -> Occ(f) = 0 /\ Runs(f) + Drains(f) = 1
          [] state[f] = "parked"  -> /\ Occ(f) = 0 /\ Runs(f) + Drains(f) = 0
                                     /\ \/ f \in waiters
                                        \/ \E g \in Fibers : f \in awaiters[g] \cup wakeSet[g]
          [] state[f] = "done"    -> Occ(f) = 0 /\ Drains(f) = 0

ExactlyOnce ==
    \A m \in Msgs :
        LET n == Count(mailbox, m) + Count(recvd, m) IN
        IF pc[SenderOf(m)] = "s_send" THEN n = 0 ELSE n = 1

\* The scheduler's dispatch already leaves home_thread naming the runner.
HomeIsRunner == \A w \in Workers : running[w] \in Fibers => home[running[w]] = w

\* A pinned fiber never leaves the worker it pinned on.
PinHolds == WithGen => \A w \in Workers : (running[w] = G /\ pinned[G]) => w = pinW

\* Segments: one state each, matching the chain and the box holders.
SegStates ==
    \A s \in Segs :
        /\ (segState[s] = "running") <=> (s \in Chain)
        /\ (segState[s] = "suspended") <=> (holder[s] # None)
CleanupOnce == \A s \in Segs : cleanup[s] = IF segState[s] = "dead" THEN 1 ELSE 0
StackOnOneWorker == Cardinality({w \in Workers : running[w] = G /\ ctx \in Segs}) <= 1
ParentAcyclic == ReachesStack(ctx, NSegs)
\* Abandon and Cancel leave nothing of G's segments behind.
SegsDeadAtExit == (WithGen /\ state[G] = "done") => \A s \in Segs : segState[s] \in {"none", "dead"}

-----------------------------------------------------------------------------
(* Reachability witnesses: each must be violated, or the search never      *)
(* reached the shape the invariants above are meant to cover.              *)

WitnessStolen == ~(\E w \in Workers : running[w] = R /\ pc[R] = "use" /\ cache[R] # w)
WitnessPermit == ~(\E w \in Workers : drainK[w] = "park" /\ wakePending[drainF[w]] > 0)
WitnessSegmentMigrates ==
    ~(\E w \in Workers : running[w] = G /\ pc[G] = "use" /\ ctx \in Segs /\ cache[G] # w)
WitnessNested == ~(Segs # {} /\ \A s \in Segs : segState[s] = "running")
WitnessHeldChain == ~(\E s, t \in Segs : holder[t] = s /\ segState[s] = "suspended")
WitnessYieldWhilePinned == ~(WithGen /\ pinned[G] /\ pc[G] = "use")
WitnessCancelCrossesSegment == ~(escTo = Pad /\ ctx \in Segs)
WitnessAbandonToSegment == ~(escTo \in Segs)

-----------------------------------------------------------------------------
(* Liveness, under FairSpec *)

RunnableRuns == \A f \in Fibers : state[f] = "ready" ~> state[f] = "running"
ParkedWithMessageResumes == (state[R] = "parked" /\ mailbox # <<>>) ~> state[R] = "running"
AwaiterResumes == (state[S2] = "parked" /\ state[S1] = "done") ~> state[S2] = "running"
AllDelivered == <>(Len(recvd) = 2)
AllExit == <>(\A f \in Fibers : pc[f] = "exited")
=============================================================================
