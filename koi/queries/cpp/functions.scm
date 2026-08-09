(function_definition
  declarator: (function_declarator declarator: (_) @function.name)) @function.def

(function_definition
  declarator: (reference_declarator
    (function_declarator
      declarator: (identifier) @function.name))) @function.def

(function_definition
  declarator: (pointer_declarator
    (function_declarator
      declarator: (identifier) @function.name))) @function.def
