; A table header is what a TOML file's structure is made of; a bare key at the
; top of a pair is what you actually look one up by.

[
  (table (bare_key) @definition)
  (table (dotted_key) @definition)
  (table_array_element (bare_key) @definition)
  (table_array_element (dotted_key) @definition)
  (pair (bare_key) @definition)
  (pair (dotted_key) @definition)
]
