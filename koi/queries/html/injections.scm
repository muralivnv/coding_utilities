; A <script> or <style> body is not HTML. The html grammar hands it back as one
; opaque (raw_text) node, so without this the whole block draws as plain text --
; the same shape as markdown's (inline), one level out.

((script_element
  (raw_text) @injection.content)
 (#set! injection.language "javascript"))

((style_element
  (raw_text) @injection.content)
 (#set! injection.language "css"))
