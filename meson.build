project(
	'wlvncc',
	'c',
	version: '0.1.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

buildtype = get_option('buildtype')
host_system = host_machine.system()
prefix = get_option('prefix')

c_args = [
	'-D_GNU_SOURCE',
]

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)
librt = cc.find_library('rt', required: false)

xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
wayland_client = dependency('wayland-client')
libvncclient = dependency('libvncclient')

aml_project = subproject('aml', required: false)
if aml_project.found()
	aml = aml_project.get_variable('aml_dep')
else
	aml = dependency('aml')
endif

inc = include_directories('include')

subdir('protocols')

sources = [
	'src/main.c',
	'src/shm.c',
	'src/seat.c',
	'src/pointer.c',
	'src/keyboard.c',
	'src/vnc.c',
	'src/strlcpy.c',
]

dependencies = [
	libm,
	librt,
	xkbcommon,
	pixman,
	aml,
	wayland_client,
	libvncclient,
	client_protos,
]

config = configuration_data()

config.set('PREFIX', '"' + prefix + '"')

if host_system == 'linux' and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

configure_file(
	output: 'config.h',
	configuration: config,
)

executable(
	'wvncc',
	sources,
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)
