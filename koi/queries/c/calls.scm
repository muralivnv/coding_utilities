; Simple call: foo()
(call_expression function: (identifier) @call)

; Member call through a struct: obj.fn() / p->fn()
(call_expression function: (field_expression field: (field_identifier) @call))
