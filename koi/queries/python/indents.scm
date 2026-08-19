; From helix 079a789e8cb08ead67f19e1971a1b7438b37354b, with koi additions below the marker.
[
  (list)
  (tuple)
  (dictionary)
  (set)

  (if_statement)
  (for_statement)
  (while_statement)
  (with_statement)
  (try_statement)
  (match_statement)
  (case_clause)
  (import_from_statement)

  (parenthesized_expression)
  (generator_expression)
  (list_comprehension)
  (set_comprehension)
  (dictionary_comprehension)

  (tuple_pattern)
  (list_pattern)
  (argument_list)
  (parameters)
  (binary_operator)

  (function_definition)
  (class_definition)
] @indent

; Workaround for the tree-sitter grammar creating large errors when a
; try_statement is missing the except/finally clause
(ERROR
  "try"
  .
  ":" @indent @extend)
(ERROR
  .
  "def") @indent @extend
(ERROR
  (block) @indent @extend
  )

[
  (if_statement)
  (for_statement)
  (while_statement)
  (with_statement)
  (try_statement)
  (match_statement)
  (case_clause)

  (function_definition)
  (class_definition)

  (except_clause)
  (finally_clause)
] @extend

[
  (return_statement)
  (break_statement)
  (continue_statement)
  (raise_statement)
  (pass_statement)
] @extend.prevent-once

[
  ")"
  "]"
  "}"
] @outdent
(elif_clause
  "elif" @outdent)
(else_clause
  "else" @outdent)
(except_clause
  "except" @outdent)
(finally_clause
  "finally" @outdent)

(parameters
  .
  (identifier) @anchor
  ) @align
(argument_list
  .
  (_) @anchor
  ) @align

; String bodies (triple-quoted strings span lines) are literal content.
(string) @opaque

; ---------------------------------------------------------------------------
; koi additions. Above this marker is the file as it was vendored; below it is
; koi's own, and is in no helix runtime.
; ---------------------------------------------------------------------------
;
; `else:` typed into a body that has nothing under it yet. The clause is not an
; `else_clause` -- with no block to attach it to the grammar recovers the
; `if`'s `:`, the body and the `else` as one ERROR, and the `(else_clause
; "else" @outdent)` rule above has nothing to match -- so the keyword stayed in
; the column the body was written at until a line was typed beneath it. The
; `elif`, `except` and `finally` clauses all recover as real nodes and need no
; help; `else` is the one that does. Naming the recovery shape is the same
; workaround the `try` and `def` rules above already use.
(ERROR
  ":"
  "else" @outdent)
