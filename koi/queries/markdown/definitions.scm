; A markdown file's symbols are its headings, which makes the symbol picker a
; table of contents. The marker is part of the captured text, so `## Design`
; sorts and reads as the level-two heading it is.

[
  (atx_heading) @definition
  (setext_heading (paragraph) @definition)
]
