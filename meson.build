# SPDX-License-Identifier: Apache-2.0
project('runway', 'c',
  version : '0.1',
  meson_version : '>= 1.0',
  default_options : ['c_std=gnu11', 'warning_level=2', 'werror=true', 'optimization=2', 'b_pie=false'])

cc = meson.get_compiler('c')
arch = host_machine.cpu_family()
if arch != 'x86_64' and arch != 'aarch64'
  error('Unsupported architecture: ' + arch)
endif
gnuefi_inc = get_option('gnuefi_inc')
gnuefi_lib = get_option('gnuefi_lib')

# Host tools and tests run on the build machine, unless native_tools is off in a cross build.
native_tools = get_option('native_tools')
# The tools are installed when they are made for the host machine.
install_tools = not meson.is_cross_build() or not native_tools
host_inc = include_directories('include', 'tools', 'src')
libm = meson.get_compiler('c', native : native_tools).find_library('m')
openssl = dependency('libcrypto', native : native_tools)

subdir('src')
subdir('tools')

subdir('efi')
subdir('fuse')
subdir('tests/host')
subdir('tests/qemu')
subdir('demo')
subdir('data')
