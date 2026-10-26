Type definitions that hold a text slot beside an id slot, and what the text is.

  file  type  why

None of these texts names the thing the id identifies:
  binder    the place a local is declared: its id and the one copy of the name it was written with
  op label  an effect or protocol operation's label; an operation has no id of its own
  origin    the module a declaration or a piece of code comes from (`None`: the root file)
  emitted   text an emitter spells: a C symbol, a register, a dispatch key, a run-time name
  tag       a closed tag or a message, not a name
  key       text another table is keyed by, with the id as its value
