# SPDX-License-Identifier: Apache-2.0
fuse3 = dependency('fuse3', required : get_option('fuse'), native : native_tools)

rwy_fuse = disabler()
if fuse3.found()
  rwy_fuse = executable('rwy-fuse',
    ['rwy_fuse.c', 'rwy_fuse_virt.c', 'rwy_fuse_store.c',
     '../tools/rwyctl_cmd.c', '../tools/rwyctl_rpc.c',
     '../tools/rwyctl_install.c', '../tools/rwyctl_write.c',
     '../tools/rwyctl_read.c', '../tools/rwyctl_file.c',
     '../tools/rwyctl_anchor.c', '../tools/rwyctl_mirror.c',
     '../tools/rwyctl_check.c', '../tools/rwyctl_blob.c',
     '../tools/rwyctl_transp.c', '../tools/rwyctl_netif.c',
     files('../tools/rwyctl_open.c', '../tools/anchor_linux.c', '../tools/guard_linux.c', '../tools/freeze_linux.c'),
     tpm_src, recarea_src, loader_src],
    include_directories : host_inc,
    c_args : '-Wno-format-truncation',
    link_with : [hostdev_lib, core_lib, crypto_lib, crypto_openssl_lib],
    dependencies : [fuse3, openssl, libm], native : native_tools,
    install : install_tools)
  install_man('../man/rwy-fuse.1')
endif

