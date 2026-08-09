; Simple call: foo()
(call_expression function: (identifier) @call)

; Qualified call: ns::func()
(call_expression function: (qualified_identifier) @call)

; Method call: obj.method() - captures just the method name
(call_expression function: (field_expression field: (field_identifier) @call))

; Unqualified template call: func<T>()
(call_expression function: (template_function name: (identifier) @call))
