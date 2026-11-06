# SPDX-License-Identifier: Apache-2.0
hostdev_lib = static_library('hostdev', 'hostdev.c', 'partuuid.c',
  include_directories : host_inc, c_args : '-Wno-format-truncation', native : native_tools)

tpm_src = files('tpm_linux.c', '../src/rwy_tpm_cmd.c')
recarea_src = files('../src/rwy_recarea.c', '../src/rwy_bundle.c')
loader_src = files('../src/rwy_loader_fsm.c')

fsck_runway = executable('fsck.runway',
  files('fsck/bootcfg.c', 'fsck/file.c', 'fsck/main.c', 'fsck/meta.c', 'fsck/repair.c',
        'fsck/run.c', 'fsck/sb.c', 'fsck/session.c', 'fsck/tree.c'),
  tpm_src, recarea_src,
  include_directories : host_inc,
  link_with : [hostdev_lib, core_lib, crypto_lib, crypto_openssl_lib],
  dependencies : openssl, native : native_tools,
  install : install_tools,
  install_dir : get_option('sbindir'))

rwyctl_sources = files('rwyctl.c', 'rwyctl_cmd.c', 'rwyctl_rpc.c', 'rwyctl_open.c', 'rwyctl_grp.c', 'rwyctl_anchor.c', 'anchor_linux.c', 'guard_linux.c', 'freeze_linux.c', 'rwyctl_mirror.c', 'rwyctl_read.c',
  'rwyctl_install.c', 'rwyctl_blob.c', 'rwyctl_check.c', 'rwyctl_write.c', 'rwyctl_disc.c', 'rwyctl_recarea.c',
  'rwyctl_reencrypt.c', 'rwyctl_transp.c', 'rwyctl_file.c', 'rwyctl_loader.c', 'rwyctl_log.c', 'rwyctl_netif.c')

rwyctl = executable('rwyctl', rwyctl_sources, tpm_src, recarea_src, loader_src,
  include_directories : host_inc, c_args : '-Wno-format-truncation',
  link_with : [hostdev_lib, core_lib, crypto_lib, crypto_openssl_lib], dependencies : [libm, openssl], native : native_tools,
  install : install_tools)

mkstore = executable('mkstore', 'mkstore.c', recarea_src,
  include_directories : host_inc,
  link_with : [hostdev_lib, core_lib, crypto_lib, crypto_openssl_lib],
  dependencies : [libm, openssl], native : native_tools,
  install : install_tools,
  install_dir : get_option('sbindir'))

iobench_linux = executable('iobench_linux', 'iobench_linux.c',
  include_directories : host_inc, link_with : [crypto_lib], native : native_tools)

host_fs = executable('host_fs', 'host_fs.c',
  include_directories : host_inc,
  link_with : [hostdev_lib, core_lib, crypto_lib, crypto_openssl_lib],
  dependencies : [libm, openssl], native : native_tools)

host_mutate = executable('host_mutate', 'host_mutate.c',
  include_directories : host_inc, link_with : [hostdev_lib, core_lib, crypto_lib], native : native_tools)
host_cat = executable('host_cat', 'host_cat.c',
  include_directories : host_inc, link_with : [hostdev_lib, core_lib, crypto_lib], native : native_tools)

install_data('mount.runway',
  install_dir : get_option('sbindir'),
  install_mode : 'rwxr-xr-x')


install_man('../man/fsck.runway.8', '../man/mount.runway.8', '../man/rwyctl.1')
