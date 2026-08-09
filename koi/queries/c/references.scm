; Where a name is used. A reference pattern captures the whole node on purpose
; -- the row carries `foo(a, b)`, not `foo` -- because the picker filters these
; with a regex and the surrounding call is what makes one hit tellable from
; another.

[
  (call_expression function: (identifier) arguments: (argument_list)) @call
  (call_expression function: (field_expression) arguments: (argument_list)) @call

  (declaration
    declarator: (function_declarator
      declarator: (identifier)
      parameters: (parameter_list))) @function

  (declaration
    declarator: (pointer_declarator
      (function_declarator
        declarator: (identifier)
        parameters: (parameter_list)))) @function

  (declaration type: (type_identifier) @type)
]
