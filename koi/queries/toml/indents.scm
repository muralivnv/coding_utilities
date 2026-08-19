; From helix 079a789e8cb08ead67f19e1971a1b7438b37354b, verbatim.
; TOML keeps table/array-of-table bodies flat (keys at column 0), so only
; multi-line value arrays and inline tables indent; their closers dedent.
[
  (array)
  (inline_table)
] @indent

[
  "]"
  "}"
] @outdent
