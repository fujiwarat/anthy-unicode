libordering_sources = [
  'candswap.c',
  'candsort.c',
  'commit.c',
  'relation.c',
  'infosort.c',
  'candhistory.c',
]

libordering = static_library('ordering',
  sources: libordering_sources,
  include_directories: [top_srcdir],
)

ordering_dep = declare_dependency(link_with: libordering)
