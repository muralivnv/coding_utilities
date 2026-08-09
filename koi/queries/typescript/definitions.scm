; Not `; inherits: javascript`, tempting as that is: the two grammars disagree
; about the node under `class_declaration name:` -- an identifier in JavaScript,
; a type_identifier in TypeScript -- and a query naming the wrong one does not
; degrade, it fails to compile at all.

[
  (function_declaration name: (identifier) @definition)
  (generator_function_declaration name: (identifier) @definition)
  (class_declaration name: (type_identifier) @definition)
  (abstract_class_declaration name: (type_identifier) @definition)
  (method_definition name: (property_identifier) @definition)
  (variable_declarator name: (identifier) @definition)
  (interface_declaration name: (type_identifier) @definition)
  (type_alias_declaration name: (type_identifier) @definition)
  (enum_declaration name: (identifier) @definition)
  (public_field_definition name: (property_identifier) @definition)
  (module name: (identifier) @definition)
]
