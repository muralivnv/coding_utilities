; Member variables
(field_declaration declarator: (field_identifier) @member)
(field_declaration declarator: (pointer_declarator declarator: (field_identifier) @member))
(field_declaration declarator: (reference_declarator (field_identifier) @member))

; Method declarations
(field_declaration declarator: (function_declarator declarator: (_) @method))
(field_declaration declarator: (reference_declarator (function_declarator declarator: (_) @method)))
(field_declaration declarator: (pointer_declarator (function_declarator declarator: (_) @method)))

; Inline method definitions
(function_definition declarator: (function_declarator declarator: (_) @method))
(function_definition declarator: (reference_declarator (function_declarator declarator: (_) @method)))
(function_definition declarator: (pointer_declarator (function_declarator declarator: (_) @method)))
