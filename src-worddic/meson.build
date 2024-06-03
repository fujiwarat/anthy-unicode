libanthydic_sources = [
  'word_dic.c',
  'dic_util.c',
  'wtype.c',
  'texttrie.c',
  'textdict.c',
  'record.c',
  'word_lookup.c',
  'use_dic.c',
  'priv_dic.c',
  'mem_dic.c',
  'ext_ent.c',
  'matrix.c',
  'feature_set.c',
]

libanthydic_unicode = library('anthydic-unicode',
  sources: libanthydic_sources,
  include_directories: [top_srcdir],
  dependencies: [diclib_dep],
  version: '0.1.0',
  install: true,
)

anthydic_dep = declare_dependency(link_with: libanthydic_unicode)
