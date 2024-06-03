libdiclib_sources = [
  'diclib.c',
  'file_dic.c',
  'filemap.c',
  'xstr.c',
  'xchar.c',
  'alloc.c',
  'conf.c',
  'logger.c',
  'ruleparser.c',
]

libdiclib = static_library('diclib',
  sources: libdiclib_sources,
  include_directories: [top_srcdir],
)

diclib_dep = declare_dependency(link_with: libdiclib)
