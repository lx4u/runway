# SPDX-License-Identifier: Apache-2.0
core_names = [
  'rwy_util', 'rwy_mirror', 'rwy_io', 'rwy_alloc', 'rwy_gc', 'rwy_mount', 'rwy_record',
  'rwy_anchor', 'rwy_txn', 'rwy_defrag', 'rwy_relocate', 'rwy_inode', 'rwy_dir', 'rwy_ns', 'rwy_file',
  'rwy_xattr', 'rwy_oneshot', 'rwy_virt', 'rwy_slotstate', 'rwy_bootcfg', 'rwy_bootsel',
  'rwy_bootaudit', 'rwy_flight', 'rwy_netset', 'rwy_transplog', 'rwy_entry', 'rwy_meta',
  'rwy_bls_name', 'rwy_iostat', 'rwy_loopguard',
]
crypto_names = [
  'rwy_crypto', 'rwy_cryptodev', 'rwy_keyslot', 'rwy_sha256', 'rwy_aes', 'rwy_xts',
  'rwy_reencrypt',
]

core_src = []
foreach n : core_names
  core_src += files(n + '.c')
endforeach
crypto_src = []
foreach n : crypto_names
  crypto_src += files(n + '.c')
endforeach

core_host_names = [
  'rwy_bls_parse',
  'rwy_bls_render',
  'rwy_bls_view',
  'rwy_bls_commit',
  'rwy_blob',
  'rwy_bootblob',
]
core_host_src = []
foreach n : core_host_names
  core_host_src += files(n + '.c')
endforeach

core_lib = static_library('rwy_core', core_src + core_host_src,
  include_directories : host_inc, native : native_tools)
# The EFI application has no use for the DRBG; host tools seed their keys with it.
crypto_lib = static_library('rwy_crypto', crypto_src, files('rwy_drbg.c'),
  include_directories : host_inc, native : native_tools)

# The OpenSSL backend of the crypto interface, for the host only.
crypto_openssl_lib = static_library('rwy_crypto_openssl', '../tools/crypto_openssl.c',
  include_directories : host_inc, dependencies : openssl, native : native_tools)
