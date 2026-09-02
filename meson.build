project('v4l2stateless', 'c',
  version: '0.1.0',
  license: 'LGPL-2.1-or-later',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')

libva_dep = dependency('libva', version: '>= 1.20.0')
libva_drm_dep = dependency('libva-drm', version: '>= 1.20.0')
libdrm_dep = dependency('libdrm', version: '>= 2.4.0')
gbm_dep = dependency('gbm')

# Get the driver install directory from libva
libva_driverdir = libva_dep.get_variable(pkgconfig: 'driverdir')
if libva_driverdir == ''
  libva_driverdir = get_option('libdir') / 'dri'
endif

add_project_arguments(
  '-D_GNU_SOURCE',
  '-DVA_DRIVER_INIT_FUNC_NAME=__vaDriverInit_1_20',
  language: 'c',
)

inc = include_directories('src')

core_sources = files(
  'src/v4l2stateless.c',
  'src/v4l2stateless_device.c',
  'src/v4l2stateless_probe.c',
  'src/v4l2stateless_config.c',
  'src/v4l2stateless_context.c',
  'src/v4l2stateless_buffer.c',
  'src/v4l2stateless_h264.c',
  'src/v4l2stateless_hevc.c',
  'src/v4l2stateless_av1.c',
  'src/v4l2stateless_vp8.c',
  'src/v4l2stateless_mpeg2.c',
  'src/v4l2stateless_format.c',
  'src/v4l2stateless_gbm.c',
  'src/v4l2stateless_jpeg.c',
  'src/v4l2stateless_vpp.c',
)

# One source of truth: every driver source lives in this static library; the
# .so and all test executables link it. link_whole on the shared library is
# REQUIRED — plain link_with would drop unreferenced objects (the codec
# translators) from the .so.
libv4l2sl_core = static_library('v4l2sl_core', core_sources,
  include_directories: inc,
  dependencies: [libva_dep, libva_drm_dep, libdrm_dep, gbm_dep],
)

shared_library('v4l2stateless_drv_video',
  [],
  link_whole: libv4l2sl_core,
  install: true,
  install_dir: libva_driverdir,
  name_prefix: '',  # No 'lib' prefix
)

test_video_probe = executable('test_video_probe',
  files('tests/test_video_probe.c', 'src/v4l2stateless_probe.c'),
  include_directories: inc,
  dependencies: [libva_dep],
)

test('video_probe', test_video_probe)

test_vp8_mpeg2_fill = executable('test_vp8_mpeg2_fill',
  files('tests/test_vp8_mpeg2_fill.c'),
  link_with: libv4l2sl_core,
  include_directories: inc,
  dependencies: [libva_dep, libdrm_dep, gbm_dep],
)

test('vp8_mpeg2_fill', test_vp8_mpeg2_fill)

test_export_recapture = executable('test_export_recapture',
  files('tests/test_export_recapture.c'),
  link_with: libv4l2sl_core,
  include_directories: inc,
  dependencies: [libva_dep, libdrm_dep, gbm_dep],
)

test('export_recapture', test_export_recapture)

test_format = executable('test_format',
  files('tests/test_format.c'),
  link_with: libv4l2sl_core,
  include_directories: inc,
  dependencies: [libva_dep, libdrm_dep, gbm_dep],
)

test('format', test_format)

va_export_client = executable('va_export_client',
  files('tests/va_export_client.c'),
  dependencies: [libva_dep, libva_drm_dep, libdrm_dep,
                 dependency('gbm'), dependency('egl'), dependency('glesv2')],
)
