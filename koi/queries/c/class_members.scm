; Struct fields
(field_declaration declarator: (field_identifier) @member)
(field_declaration declarator: (pointer_declarator declarator: (field_identifier) @member))

; Function-pointer members read as methods
(field_declaration declarator: (function_declarator declarator: (_) @method))
(field_declaration declarator: (pointer_declarator (function_declarator declarator: (_) @method)))
