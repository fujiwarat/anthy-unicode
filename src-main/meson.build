libanthy_sources = [
  'main.c',
  'context.c',
]

libanthy_unicode = library('anthy-unicode',
  sources: libanthy_sources,
  include_directories: [top_srcdir],
  dependencies: [anthydic_dep, ordering_dep, split_dep],
  version: '0.1.0',
  install: true,
)

anthy_dep = declare_dependency(link_with: libanthy_unicode)
