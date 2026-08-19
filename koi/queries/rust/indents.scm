; From helix 079a789e8cb08ead67f19e1971a1b7438b37354b, with koi additions below the marker.
[
  (use_list)
  (block)
  (match_block)
  (arguments)
  (parameters)
  (declaration_list)
  (field_declaration_list)
  (field_initializer_list)
  (struct_pattern)
  (tuple_pattern)
  (unit_expression)
  (enum_variant_list)
  (call_expression)
  (binary_expression)
  (field_expression)
  (await_expression)
  (tuple_expression)
  (array_expression)
  (where_clause)
  (type_cast_expression)

  (token_tree)
  (macro_definition)
  (token_tree_pattern)
  (token_repetition)
] @indent

[
  "}"
  "]"
  ")"
] @outdent

; Indent the right side of assignments.
; The #not-same-line? predicate is required to prevent an extra indent for e.g.
; an else-clause where the previous if-clause starts on the same line as the assignment.
(assignment_expression
  .
  (_) @expr-start
  right: (_) @indent
  (#not-same-line? @indent @expr-start)

)
(compound_assignment_expr
  .
  (_) @expr-start
  right: (_) @indent
  (#not-same-line? @indent @expr-start)

)
(let_declaration
  "let" @expr-start
  value: (_) @indent
  alternative: (_)? @indent
  (#not-same-line? @indent @expr-start)

)
(let_condition
  .
  (_) @expr-start
  value: (_) @indent
  (#not-same-line? @indent @expr-start)

)
(if_expression
  .
  (_) @expr-start
  condition: (_) @indent
  (#not-same-line? @indent @expr-start)

)
(static_item
  .
  (_) @expr-start
  value: (_) @indent
  (#not-same-line? @indent @expr-start)

)
(field_pattern
  .
  (_) @expr-start
  pattern: (_) @indent
  (#not-same-line? @indent @expr-start)

)
; Indent type aliases that span multiple lines, similar to
; regular assignment expressions
(type_item
  .
  (_) @expr-start
  type: (_) @indent
  (#not-same-line? @indent @expr-start)

)

; Some field expressions where the left part is a multiline expression are not
; indented by cargo fmt.
; Because this multiline expression might be nested in an arbitrary number of
; field expressions, this can only be matched using a Regex.
(field_expression
  value: (_) @val
  "." @outdent
  ; Check whether the first line ends with `(`, `{` or `[` (up to whitespace).
  (#match? @val "(\\A[^\\n\\r]+(\\(|\\{|\\[)[\\t ]*(\\n|\\r))")
)
; Same as above, but with an additional `call_expression`. This is required since otherwise
; the arguments of the function call won't be outdented.
(call_expression
  function: (field_expression
    value: (_) @val
    "." @outdent
    (#match? @val "(\\A[^\\n\\r]+(\\(|\\{|\\[)[\\t ]*(\\n|\\r))")
  )
  arguments: (_) @outdent
)
; Same again for `.await` off a multiline receiver, so it lines up with the
; other links in the chain instead of indenting a level deeper.
(await_expression
  (_) @val
  "." @outdent
  (#match? @val "(\\A[^\\n\\r]+(\\(|\\{|\\[)[\\t ]*(\\n|\\r))")
)

; Indent if guards in patterns.
; Since the tree-sitter grammar doesn't create a node for the if expression,
; it's not possible to do this correctly in all cases. Indenting the tail of the
; whole pattern whenever it contains an `if` only fails if the `if` appears after
; the second line of the pattern (which should only rarely be the case)
(match_pattern
  .
  (_) @expr-start
  "if" @pattern-guard
  (#not-same-line? @expr-start @pattern-guard)
) @indent

; Align closure parameters if they span more than one line
(closure_parameters
  "|"
  .
  (_) @anchor
  (_) @expr-end
  .
  (#not-same-line? @anchor @expr-end)
) @align

(for_expression
  "in" @in
  .
  (_) @indent
  (#not-same-line? @in @indent)

)

; Multi-line string / raw-string bodies are literal content: preserve them.
[
  (string_literal)
  (raw_string_literal)
] @opaque

; ---------------------------------------------------------------------------
; koi additions. Above this marker is the file as it was vendored; below it is
; koi's own, and is in no helix runtime.
; ---------------------------------------------------------------------------
;
; Continuation lines. The `#not-same-line?` family above opens its scope on the
; node it captures -- the value, the right-hand side -- and under koi's
; containment engine a scope only indents the lines *below* the one it opens
; on, so a value written on its own line indented nothing at all: `let y =`
; followed by `2;` left the `2;` at the block's own column. Re-stating each of
; them with `(#set! "scope" "header")` opens the scope at the parent instead --
; the `let`, the assignment, the `static` -- which is the line the continuation
; is a continuation *of*, so the value's own line is the first one inside it.
;
; The predicates are unchanged, so a value that begins on the parent's line
; still matches nothing here; and a value that is itself an `@indent` node (a
; block, a call, an array) collapses into one level rather than two, because a
; scope contributes at most one level per line it opens on and these two open
; on the same one.
(assignment_expression
  .
  (_) @expr-start
  right: (_) @indent
  (#not-same-line? @indent @expr-start)
  (#set! "scope" "header"))
(compound_assignment_expr
  .
  (_) @expr-start
  right: (_) @indent
  (#not-same-line? @indent @expr-start)
  (#set! "scope" "header"))
(let_declaration
  "let" @expr-start
  value: (_) @indent
  (#not-same-line? @indent @expr-start)
  (#set! "scope" "header"))
(let_condition
  .
  (_) @expr-start
  value: (_) @indent
  (#not-same-line? @indent @expr-start)
  (#set! "scope" "header"))
(static_item
  .
  (_) @expr-start
  value: (_) @indent
  (#not-same-line? @indent @expr-start)
  (#set! "scope" "header"))
(type_item
  .
  (_) @expr-start
  type: (_) @indent
  (#not-same-line? @indent @expr-start)
  (#set! "scope" "header"))

; The other half of the same gap, and the half the rules above cannot reach: a
; binding whose value has not been typed yet. `let y =` is not a
; `let_declaration` at all -- the grammar recovers it as an ERROR holding the
; `let`, the name and the `=` -- so `value: (_)` binds nothing, the newline that
; opens the continuation gets nothing from any rule, and the line the user then
; writes lands a level short. The hybrid heuristic then reads that line as its
; baseline and turns one wrong level into two: the *next* statement comes out at
; column zero. Naming the recovery node is what python's own file already does
; for `try:` and `def`, and `@extend` is what lets a node that stops above the
; line still own it.
(ERROR
  "let"
  "=") @indent @extend
