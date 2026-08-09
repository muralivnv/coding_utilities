; Where a name is introduced in C. The C++ file is a superset of this one
; rather than an `; inherits:` of it: inheriting would make every C++ file match
; the shared patterns twice and list every function two times over.

[
  (function_definition declarator: (function_declarator declarator: (_) @definition))
  (function_definition
    declarator: (pointer_declarator
      (function_declarator declarator: (identifier) @definition)))

  (enum_specifier name: (type_identifier) @definition)
  (struct_specifier name: (type_identifier) @definition)
  (union_specifier name: (type_identifier) @definition)
  (enumerator name: (identifier) @definition)
  (type_definition declarator: (type_identifier) @definition)
  (preproc_def name: (identifier) @definition)
  (preproc_function_def name: (identifier) @definition)
]
