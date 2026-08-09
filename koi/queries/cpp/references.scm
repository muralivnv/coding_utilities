[
  (call_expression function: (field_expression) arguments: (argument_list)) @call
  (call_expression function: (identifier) arguments: (argument_list)) @call
  (call_expression function: (qualified_identifier) arguments: (argument_list)) @call

  (field_declaration declarator: (function_declarator parameters: (parameter_list))) @function

  (field_declaration
    declarator: (reference_declarator
      (function_declarator
        declarator: (field_identifier)
        parameters: (parameter_list)))) @function

  (field_declaration
    declarator: (pointer_declarator
      (function_declarator
        declarator: (field_identifier)
        parameters: (parameter_list)))) @function
  
  (declaration
    declarator: (function_declarator
      declarator: (identifier)
      parameters: (parameter_list))) @function

  (declaration
    declarator: (reference_declarator
      (function_declarator
        declarator: (_)
        parameters: (parameter_list)))) @function

  (declaration
    declarator: (pointer_declarator
      (function_declarator
        declarator: (identifier) @function.name)))

  (declaration type: (type_identifier) @type)
]

