libanthyinput_sources = [
  'input.c',
  'rkconv.c',
  'rkhelper.c',
]

anthy_agent_sources = [
  'agent.c',
  'egg.c',
]

elisp_sources = [
  'anthy-unicode.el',
  'anthy-unicode-azik.el',
  'anthy-unicode-conf.el',
  'anthy-unicode-dic.el',
  'anthy-unicode-isearch.el',
  'anthy-unicode-kyuri.el',
  'leim-list.el',
]

libconvdb = static_library('convdb',
  sources: 'convdb.c',
  include_directories: [top_srcdir],
)

libanthyinput = library('anthyinput-unicode',
  sources: libanthyinput_sources,
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  version: '0.0.0',
  install: true,
)

convdb_dep     = declare_dependency(link_with: libconvdb)
anthyinput_dep = declare_dependency(link_with: libanthyinput)

anthy_dic_tool = executable('anthy-dic-tool-unicode',
  sources: 'dic-tool.c',
  include_directories: [top_srcdir],
  dependencies: [anthydic_dep],
  install: true,
)

anthy_agent = executable('anthy-agent-unicode',
  sources: anthy_agent_sources,
  include_directories: [top_srcdir],
  dependencies: [anthyinput_dep, anthy_dep, anthydic_dep],
  install: true,
)

anthy_morpholigical_analyzer = executable('anthy-morphological-analyzer-unicode',
  sources: 'morph-main.c',
  include_directories: [top_srcdir],
  dependencies: [convdb_dep, anthy_dep, anthydic_dep],
  install: true,
)

install_data('typetab', install_dir : get_option('datadir') / meson.project_name())
install_data('dic-tool-usage.txt', install_dir : get_option('datadir') / meson.project_name())

if lispdir != 'no'
  foreach src : elisp_sources
    install_data(src, install_dir : lispdir / 'anthy-unicode')

    if emacs.found() and src != 'leim-list.el'
      target_name = src.split('.')[0] + '.elc'
      target_path = meson.current_build_dir() / target_name
      target_func = '(setq byte-compile-dest-file-function (lambda (_) "' + target_path + '"))'
      custom_target(target_name,
        input: src,
        output: target_name,
        install_dir: lispdir / 'anthy-unicode',
        install: true,
        # rebuild all if any changed.
        depend_files: elisp_sources,
        command: [emacs,
          '--no-init-file',
          '--batch',
          '--directory', meson.current_source_dir(),
          '--directory', meson.current_build_dir(),
          '--eval', target_func,
          '--funcall', 'batch-byte-compile',
          '@INPUT@'
        ]
      )
    endif
  endforeach
endif
