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

sources = files(
  'src/v4l2stateless.c',
  'src/v4l2stateless_device.c',
  'src/v4l2stateless_config.c',
  'src/v4l2stateless_context.c',
  'src/v4l2stateless_buffer.c',
  'src/v4l2stateless_h264.c',
  'src/v4l2stateless_hevc.c',
  'src/v4l2stateless_av1.c',
)

shared_library('v4l2stateless_drv_video',
  sources,
  dependencies: [libva_dep, libva_drm_dep, libdrm_dep],
  install: true,
  install_dir: libva_driverdir,
  name_prefix: '',  # No 'lib' prefix
)
