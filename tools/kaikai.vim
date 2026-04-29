" Vim syntax file
" Language:     kaikai
" Maintainer:   kaikai project (https://kaikai-lang.org)
" Filenames:    *.kai
"
" Heuristic-based highlighting tracking the stage 2 lexer in
" stage2/compiler.kai. There is no semantic information: type vs.
" constructor vs. effect-label is approximated by capitalisation.
" A real LSP-driven highlighter is scheduled for milestone m17.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match

" --------------------------------------------------------------------
" Comments  ('#' to end of line; '#derive' is a token, kept distinct)
" --------------------------------------------------------------------
syn match   kaikaiDerive    "#derive\>"
syn match   kaikaiComment   "#\(derive\>\)\@!.*$" contains=kaikaiTodo
syn keyword kaikaiTodo      contained TODO FIXME XXX NOTE HACK

" --------------------------------------------------------------------
" Keywords (kept in sync with stage2/compiler.kai keyword_kind)
" --------------------------------------------------------------------
syn keyword kaikaiKeyword
      \ and as assert axiom const effect else ensures
      \ false fn for handle if impl import let match
      \ not or protocol pub requires test true type
      \ unit use var where with

syn keyword kaikaiBoolean   true false

" 'todo!' is a single token; mark it as a runtime escape.
syn match   kaikaiTodoBang  "\<todo!"

" --------------------------------------------------------------------
" Built-in primitives and well-known stdlib type names
" --------------------------------------------------------------------
syn keyword kaikaiType
      \ Int Real Bool String Char Unit Nothing
      \ Option Result List Pair Map Set Stack Queue Array
      \ Decimal Money Fiber Pid Regex Match Capture
      \ WallTime Instant Duration

" --------------------------------------------------------------------
" Built-in effect labels (Doc B catalog)
" --------------------------------------------------------------------
syn keyword kaikaiEffect
      \ Console Stdin Stdout Stderr File Env State
      \ Reader Writer Actor Spawn Cancel Mutable
      \ Fail Random Time Clock Ffi

" --------------------------------------------------------------------
" Constructors / capitalised callees and bare type names
" Heuristic: any identifier starting with an uppercase letter that
" isn't already a known type/effect highlights as a constructor.
" --------------------------------------------------------------------
syn match   kaikaiConstructor "\<[A-Z][A-Za-z0-9_]*\>"

" --------------------------------------------------------------------
" Numbers
"   Integers:  123, 1_000_000
"   Reals:     1.0, 1.5e-3, 2E10
"   Optional unit-of-measure suffix is matched separately to keep the
"   numeric literal contiguous (the '<' is matched by an operator rule).
" --------------------------------------------------------------------
syn match   kaikaiNumber    "\<\d[0-9_]*\(\.\d[0-9_]*\)\?\([eE][+-]\?\d\+\)\?\>"

" --------------------------------------------------------------------
" Strings (single-line "..." and triple-quoted """...""")
"   Both support backslash escapes and #{...} interpolation.
" --------------------------------------------------------------------
syn match   kaikaiEscape    contained "\\\([\\\"'nrt0]\|u{[0-9a-fA-F]\+}\)"
syn region  kaikaiInterp    contained matchgroup=kaikaiInterpDelim
      \ start="#{" end="}"
      \ contains=kaikaiNumber,kaikaiKeyword,kaikaiBoolean,kaikaiType,
      \ kaikaiEffect,kaikaiConstructor,kaikaiOperator,kaikaiString,
      \ kaikaiTripleString,kaikaiChar
syn region  kaikaiTripleString start=+"""+ end=+"""+
      \ contains=kaikaiEscape,kaikaiInterp
syn region  kaikaiString    start=+"+ skip=+\\.+ end=+"+ oneline
      \ contains=kaikaiEscape,kaikaiInterp

" --------------------------------------------------------------------
" Char literals: 'c', '\n', '\u{1F600}'
" --------------------------------------------------------------------
syn match   kaikaiChar      "'\(\\\(u{[0-9a-fA-F]\+}\|.\)\|[^\\']\)'"

" --------------------------------------------------------------------
" Typed holes: ?  and ?name
" --------------------------------------------------------------------
syn match   kaikaiHole      "?\([a-zA-Z_][a-zA-Z0-9_]*\)\?"

" --------------------------------------------------------------------
" Operators and punctuation
" --------------------------------------------------------------------
syn match   kaikaiOperator  "->\|=>\|||>\||>\|::\|:=\|==\|!=\|<=\|>=\|&&\|||\|++\|\.\.\.\|\.\."
syn match   kaikaiOperator  "[-+*/%<>=!?^|@:]"

" Pipe character on its own (sum-type alternation / handle clause).
syn match   kaikaiPipe      "|"

" --------------------------------------------------------------------
" fn / type / effect declaration heads — make the bound name stand out
" --------------------------------------------------------------------
syn match   kaikaiFnName    "\(\<fn\s\+\)\@<=[a-z_][A-Za-z0-9_]*"
syn match   kaikaiTypeDecl  "\(\<\(type\|effect\|protocol\|impl\)\s\+\)\@<=[A-Z][A-Za-z0-9_]*"

" --------------------------------------------------------------------
" Highlight links
" --------------------------------------------------------------------
hi def link kaikaiComment      Comment
hi def link kaikaiTodo         Todo
hi def link kaikaiDerive       PreProc
hi def link kaikaiKeyword      Keyword
hi def link kaikaiBoolean      Boolean
hi def link kaikaiTodoBang     Special
hi def link kaikaiType         Type
hi def link kaikaiEffect       StorageClass
hi def link kaikaiConstructor  Identifier
hi def link kaikaiNumber       Number
hi def link kaikaiString       String
hi def link kaikaiTripleString String
hi def link kaikaiEscape       SpecialChar
hi def link kaikaiInterpDelim  Special
hi def link kaikaiChar         Character
hi def link kaikaiHole         Special
hi def link kaikaiOperator     Operator
hi def link kaikaiPipe         Operator
hi def link kaikaiFnName       Function
hi def link kaikaiTypeDecl     Type

let b:current_syntax = "kaikai"

let &cpo = s:cpo_save
unlet s:cpo_save
