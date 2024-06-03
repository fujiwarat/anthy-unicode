project('anthy-unicode',
  ['c'],
  version: '1.0.0.20240207',
  license: 'LGPL2.1+',
  meson_version: '>= 0.56',
)

cc = meson.get_compiler('c')

config_h = configuration_data()
config_h.set('VERSION', '"'+meson.project_version()+'"')
config_h.set('PACKAGE', '"'+meson.project_name()+'"')

foreach a : ['strerror_r']
  if cc.has_function(a)
    config_h.set('HAVE_'+a.to_upper(), 1,
      description: 'Define to 1 if you have the `'+a+'\' function.')
  endif
endforeach

configure_file(output: 'config.h', configuration: config_h)
add_project_arguments('-DCONF_DIR="'+get_option('sysconfdir')+'"', language : 'c')

if get_option('lispdir') != 'no' and get_option('emacs') != 'no'
  if get_option('emacs') != ''
    emacs = find_program(get_option('emacs'), required: true)
  else
    emacs = find_program('emacs', required: false)
  endif
else
  emacs = disabler()
endif

if get_option('lispdir') == ''
  lispdir = ''
  if emacs.found()
    r = run_command(emacs,
      '-batch',
      '-Q',
      '-eval',
      '\'(while load-path (princ (concat (car load-path) "\n")) (setq load-path (cdr load-path)))\'',
      '</dev/null',
      check: true,
    )

    foreach path : r.stdout().split('/n')
      if path.contains('emacs/site-lisp')
        if path.contains('/lib/')
          lispdir = get_option('libdir') / path.split('/lib/')[1]
        elif path.contains('/share/')
          lispdir = get_option('datadir') / path.split('/share/')[1]
        endif
      endif
    endforeach
  endif
  if lispdir == ''
    lispdir = get_option('datadir') / 'emacs' / 'site-lisp'
  endif
else
  lispdir = get_option('lispdir')
endif

top_srcdir = include_directories('.')
subdir('src-ordering')
subdir('src-diclib')
subdir('src-splitter')
subdir('src-worddic')
subdir('src-main')
subdir('src-util')
subdir('anthy')
subdir('depgraph')
subdir('mkworddic')
subdir('calctrans')
subdir('mkanthydic')
subdir('test')
#subdir('doc')
#subdir('alt-cannadic')

anthy_conf = configuration_data()
anthy_conf.set('VERSION', meson.project_version())
anthy_conf.set('PACKAGE', meson.project_name())
anthy_conf.set('datadir', '${prefix}' / get_option('datadir'))
anthy_conf.set('prefix', get_option('prefix'))
configure_file(
  input: 'anthy-unicode.conf.in',
  output: 'anthy-unicode.conf',
  configuration: anthy_conf,
  install: true,
  install_dir: get_option('sysconfdir')
)

anthy_unicode_pc = configuration_data()
anthy_unicode_pc.set('prefix', get_option('prefix'))
anthy_unicode_pc.set('includedir', '${prefix}' / get_option('includedir'))
anthy_unicode_pc.set('libdir', '${exec_prefix}' / get_option('libdir'))
anthy_unicode_pc.set('PACKAGE', meson.project_name())
anthy_unicode_pc.set('VERSION', meson.project_version())
configure_file(
  input: 'anthy-unicode.pc.in',
  output: 'anthy-unicode.pc',
  configuration: anthy_unicode_pc,
  install: true,
  install_dir: get_option('libdir') / 'pkgconfig'
)
