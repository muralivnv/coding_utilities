; From helix 079a789e8cb08ead67f19e1971a1b7438b37354b, verbatim.
[
  (if_condition)
  (foreach_loop)
  (while_loop)
  (function_def)
  (macro_def)
  (block_def)
  (normal_command)
] @indent

")" @outdent

[
  (else)
  (elseif)
  (endif)
  (endforeach)
  (endwhile)
  (endfunction)
  (endmacro)
  (endblock)
] @outdent
