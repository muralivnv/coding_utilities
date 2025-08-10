[
  (function_definition declarator: (function_declarator declarator: (_) @function.name))
  (function_definition
    declarator: (reference_declarator
      (function_declarator
        declarator: (identifier) @function.name)))

  (function_definition
    declarator: (pointer_declarator
      (function_declarator
        declarator: (identifier) @function.name)))

  (enum_specifier name: (type_identifier) @definition)
  (class_specifier name: (type_identifier) @definition)
  (struct_specifier name: (type_identifier) @definition)
  (enumerator name: (identifier) @definition)
  (alias_declaration name: (type_identifier) @definition)
  (type_definition declarator: (type_identifier) @definition)
  (preproc_def name: (identifier) @definition)
  (preproc_function_def name: (identifier) @definition)
]

