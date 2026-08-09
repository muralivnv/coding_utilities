; Markdown needs two grammars. The block grammar produces headings, lists,
; quotes and fences; everything *inside* a paragraph -- emphasis, code spans,
; links, images -- is a separate grammar that runs over the (inline) nodes the
; block grammar leaves opaque. Without this file a paragraph of prose is one
; unstyled run, which is what koi did before injections existed.

((inline) @injection.content
  (#set! injection.language "markdown_inline"))

((pipe_table_cell) @injection.content
  (#set! injection.language "markdown_inline"))

; A fenced block is highlighted as whatever its info string says it is. The
; language is a capture rather than a literal here, so it is read from the
; document; unknown spellings fall back to the block staying plain.
(fenced_code_block
  (info_string (language) @injection.language)
  (code_fence_content) @injection.content)

((html_block) @injection.content
  (#set! injection.language "html"))

; Front matter, either flavour.
((minus_metadata) @injection.content
  (#set! injection.language "yaml"))

((plus_metadata) @injection.content
  (#set! injection.language "toml"))
