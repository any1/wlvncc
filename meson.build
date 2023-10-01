project(
	'wlvncc',
	'c',
	version: '0.1.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

cmake = import('cmake')

buildtype = get_option('buildtype')
host_system = host_machine.system()
prefix = get_option('prefix')

c_args = [
	'-D_GNU_SOURCE',
	'-DAML_UNSTABLE_API=1',
]

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)
librt = cc.find_library('rt', required: false)
pthread = cc.find_library('pthread', required: false)

xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
drm = dependency('libdrm')
gbm = dependency('gbm')
egl = dependency('egl')
glesv2 = dependency('glesv2')
lavc = dependency('libavcodec')
lavu = dependency('libavutil')
gcrypt = dependency('libgcrypt', required: false)
openssl = dependency('openssl', required: false)
gnutls = dependency('gnutls', required: false)
sasl = dependency('libsasl2', required: false)
libjpeg = dependency('libjpeg', required: false)
libpng = dependency('libpng', required: false)
lzo = dependency('lzo2', required: false)
libz = dependency('zlib', required: false)

aml_project = subproject('aml', required: false,
		default_options: ['default_library=static'])
if aml_project.found()
	aml = aml_project.get_variable('aml_dep')
else
	aml = dependency('aml')
endif

inc = include_directories('include', 'src/encodings')

subdir('protocols')

sources = [
	'src/main.c',
	'src/shm.c',
	'src/seat.c',
	'src/output.c',
	'src/pointer.c',
	'src/keyboard.c',
	'src/vnc.c',
	'src/strlcpy.c',
	'src/evdev-to-qnum.c',
	'src/pixels.c',
	'src/region.c',
	'src/renderer.c',
	'src/renderer-egl.c',
	'src/buffer.c',
	'src/buffer-pool.c',
	'src/open-h264.c',
	'src/cursor.c',
	'src/rfbproto.c',
	'src/sockets.c',
	'src/vncviewer.c',
	'src/ntp.c',
	'src/performance.c',
]

dependencies = [
	libm,
	librt,
	xkbcommon,
	pixman,
	aml,
	wayland_client,
	wayland_cursor,
	drm,
	gbm,
	egl,
	glesv2,
	lavc,
	lavu,
	client_protos,
]

config = configuration_data()

config.set('PREFIX', '"' + prefix + '"')

if gcrypt.found()
	sources += 'src/crypto_libgcrypt.c'
	dependencies += gcrypt
elif openssl.found()
	sources += 'src/crypto_openssl.c'
	dependencies += openssl
else
	sources += 'src/crypto_included.c'
endif

if gnutls.found()
	sources += 'src/tls_gnutls.c'
	dependencies += gnutls
	config.set('LIBVNCSERVER_HAVE_GNUTLS', true)
elif openssl.found()
	sources += 'src/tls_openssl.c'
	dependencies += openssl
	config.set('LIBVNCSERVER_HAVE_LIBSSL', true)
else
	sources += 'src/tls_none.c'
endif

if sasl.found()
	dependencies += sasl
	sources += 'src/sasl.c'
	config.set('LIBVNCSERVER_HAVE_SASL', true)
endif

if libjpeg.found()
	sources += 'src/turbojpeg.c'
	dependencies += libjpeg
	config.set('LIBVNCSERVER_HAVE_LIBJPEG', true)
endif

if libpng.found()
	dependencies += libpng
	config.set('LIBVNCSERVER_HAVE_LIBPNG', true)
endif

if pthread.found()
	dependencies += pthread
	config.set('LIBVNCSERVER_HAVE_PTHREAD', true)
endif

if libz.found()
	dependencies += libz
	config.set('LIBVNCSERVER_HAVE_LIBZ', true)
endif

if lzo.found()
	dependencies += lzo
	config.set('LIBVNCSERVER_HAVE_LZO', true)
endif

if host_system == 'linux' and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

configure_file(
	output: 'config.h',
	configuration: config,
)

executable(
	'wlvncc',
	sources,
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)
