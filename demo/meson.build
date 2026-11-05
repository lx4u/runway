# The demo image for QEMU and real hardware. The download is separate and never part of a test run.
# SPDX-License-Identifier: Apache-2.0
rwyctl_static = executable('rwyctl-static', rwyctl_sources, tpm_src, recarea_src, loader_src,
  include_directories : host_inc, c_args : ['-Wno-format-truncation', '-DRWYCTL_BUILTIN_CRYPTO'],
  link_with : [hostdev_lib, core_lib, crypto_lib], dependencies : libm, link_args : '-static',
  native : native_tools, build_by_default : false)

demo_bin = custom_target('demo-bin',
  input : rwyctl_static, output : 'demo-bin.stamp',
  command : [mkbin, qemu_bin_dir, '@OUTPUT@', '@INPUT@'],
  build_by_default : false)

run_target('demo-fetch', command : [sh, files('fetch.sh')], env : qemu_env_normal)
run_target('demo', command : [sh, files('mkdemo.sh')], env : qemu_env_normal,
  depends : [qemu_bin, demo_bin])
test('demo', sh, args : files('test_demo.sh'), env : qemu_env_normal,
  depends : [qemu_bin, demo_bin], workdir : src_root, suite : 'demo', is_parallel : false, timeout : 1800)
