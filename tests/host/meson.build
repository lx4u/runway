# SPDX-License-Identifier: Apache-2.0
libdl = meson.get_compiler('c', native : native_tools).find_library('dl')

# The shim defines open64 and pwrite64 itself, so Meson's -D_FILE_OFFSET_BITS=64 must go.
faultio = shared_library('faultio', 'faultio.c',
  c_args : '-U_FILE_OFFSET_BITS', name_prefix : '', include_directories : host_inc, dependencies : libdl, native : native_tools)

tpmtest = executable('tpmtest',
  files('tpmtest.c', '../../tools/tpm_linux.c', '../../src/rwy_tpm_cmd.c'),
  include_directories : host_inc, link_with : [crypto_lib, core_lib], native : native_tools)

seed_store = custom_target('RUNWAY.DAT',
  input : chain_efi, output : 'RUNWAY.DAT',
  command : [files('mkseed.sh'), mkstore, '@INPUT@', '@OUTPUT@'])

testdir = files('testdir.c')
src_inc = include_directories('../../src')

# name : [sources, extra include dirs, extra link libraries, extra dependencies]
c_tests = {
  'cryptotest' : [files('cryptotest.c'), [src_inc], [crypto_lib, crypto_openssl_lib], [libm, openssl]],
  'mirrortest' : [files('mirrortest.c'), [src_inc], [], []],
  'hosttest' : [files('hosttest.c'), [src_inc], [], []],
  'committest' : [files('committest.c'), [src_inc], [], []],
  'gctest' : [files('gctest.c'), [], [], []],
  'repairtest' : [files('repairtest.c'), [], [], []],
  'defragtest' : [files('defragtest.c'), [], [], []],
  'relocatetest' : [files('relocatetest.c'), [src_inc], [], []],
  'reencrypttest' : [files('reencrypttest.c'), [src_inc], [crypto_lib, crypto_openssl_lib], [libm, openssl]],
  'xattr_link_test' : [files('xattr_link_test.c'), [], [], []],
  'scaletest' : [files('scaletest.c'), [], [], []],
  'bootcfgtest' : [files('bootcfgtest.c', 'bootcfgtest_anchor.c', 'bootcfgtest_check.c', 'bootcfgtest_fi.c',
                         'bootcfgtest_lib.c', 'bootcfgtest_loop.c', 'bootcfgtest_room.c', 'bootcfgtest_schema.c', 'bootcfgtest_select.c', 'bootcfgtest_sweep.c', 'bootcfgtest_blob.c', 'bootcfgtest_bridge.c', 'bootcfgtest_reads.c'),
                   [src_inc], [], []],
  'test_bundle' : [files('test_bundle.c', '../../src/rwy_bundle.c'), [],
                   [crypto_lib, crypto_openssl_lib], [libm, openssl]],
  'test_loader_rec' : [files('test_loader_rec.c', '../../src/rwy_loader_fsm.c', '../../src/rwy_bundle.c'), [],
                       [crypto_lib, crypto_openssl_lib], [libm, openssl]],
  'test_stub_fsm' : [files('test_stub_fsm.c', '../../src/rwy_loader_fsm.c', '../../src/rwy_bundle.c'), [],
                     [crypto_lib, crypto_openssl_lib], [libm, openssl]],
  'recareatest' : [files('recareatest.c', '../../src/rwy_recarea.c', '../../src/rwy_bundle.c'), [],
                   [crypto_lib, crypto_openssl_lib], [libm, openssl]],
  'loopguardtest' : [files('loopguardtest.c'), [src_inc], [], []],
  'flighttest' : [files('flighttest.c'), [src_inc], [], []],
  'netsettest' : [files('netsettest.c'), [src_inc], [], []],
  'transplogtest' : [files('transplogtest.c'), [src_inc], [], []],
  'blobtest' : [files('blobtest.c'), [src_inc], [crypto_openssl_lib], [openssl]],
  'blstest' : [files('blstest.c'), [src_inc], [], []],
  'blsviewtest' : [files('blsviewtest.c'), [src_inc], [], []],
  'hostdevtest' : [files('hostdevtest.c'), [], [hostdev_lib], []],
  'iobench_csv' : [files('iobench_csv_test.c', '../efi/iobench_csv.c'), [], [], []],
  'iobench_stats' : [files('iobench_stats_test.c', '../efi/iobench_stats.c'), [], [], []],
  'iobench_config' : [files('iobench_config_test.c', '../efi/iobench_config.c'), [], [], []],
  'freezetest' : [files('freezetest.c', '../../tools/freeze_linux.c'), [], [], []],
  'gpttest' : [files('gpttest.c', '../../src/rwy_gpt.c'), [], [], []],
}

test_exes = {}
test_exe_list = []
foreach name, t : c_tests
  exe = executable(name, t[0], testdir,
    include_directories : [host_inc, t[1]],
    link_with : [t[2], core_lib, crypto_lib], dependencies : t[3], native : native_tools)
  test_exes += {name : exe}
  test_exe_list += exe
endforeach

fusestoretest = executable('fusestoretest',
  files('fusestoretest.c', '../../fuse/rwy_fuse_store.c', '../../tools/rwyctl_open.c', '../../tools/anchor_linux.c', '../../tools/guard_linux.c'),
  testdir, tpm_src, include_directories : host_inc,
  link_with : [hostdev_lib, core_lib, crypto_lib, crypto_openssl_lib], dependencies : [libm, openssl], native : native_tools)
test_exes += {'fusestoretest' : fusestoretest}
test_exe_list += fusestoretest

# One directory with every program and file the scripts take from $BUILD.
fuse_bin = fuse3.found() ? [rwy_fuse] : []
bin_stamp = custom_target('bin',
  input : [fsck_runway, rwyctl, mkstore, host_fs, host_mutate, host_cat, iobench_linux, tpmtest, faultio, seed_store, chain_efi] + fuse_bin + test_exe_list,
  output : 'bin.stamp',
  command : [find_program('../mkbin.sh'), '@OUTDIR@/bin', '@OUTPUT@', '@INPUT@'],
  build_by_default : true)
bin_dir = meson.current_build_dir() / 'bin'

host_env = {'BUILD' : bin_dir, 'MKSTORE' : bin_dir / 'mkstore'}

foreach name, exe : test_exes
  test(name, exe, args : [seed_store, fsck_runway], env : host_env,
    suite : 'host', timeout : 900, depends : bin_stamp)
endforeach

test('seed-store-check', fsck_runway, args : [seed_store], suite : 'host')

# The scripts name their helper files from the source root.
sh = find_program('sh')
src_root = meson.project_source_root()
foreach s : ['rwyctl', 'evict', 'rotation', 'tpm', 'recovery_area', 'crypto_backend', 'grep_gate', 'hostdev', 'check_fat', 'fsck_options', 'iobench_collect', 'iobench_host', 'provision_whiptail', 'provision_hw', 'iobench_linux', 'stub_audit']
  test(s, sh, args : files('test_@0@.sh'.format(s)), env : host_env,
    workdir : src_root, suite : 'host', timeout : 900, depends : bin_stamp)
endforeach
test('evict-cut', sh, args : files('test_evict_cut.sh'), env : host_env + {'EVICT_CUT_STEP' : '4'},
  workdir : src_root, suite : 'host', timeout : 900, depends : bin_stamp)
test('evict-cut-full', sh, args : files('test_evict_cut.sh'), env : host_env,
  workdir : src_root, suite : 'soak', is_parallel : false, timeout : 0, depends : bin_stamp)
test('rotation-quota', sh, args : files('test_rotation.sh'), env : host_env + {'SOAK_QUOTA' : '60'},
  workdir : src_root, suite : 'host', timeout : 900, depends : bin_stamp)

if fuse3.found()
  test('fuse-quota', sh, args : files('../fuse/test_fuse_quota.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 900, depends : bin_stamp)
  test('fuse-pin', sh, args : files('../fuse/test_fuse_pin.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 900, depends : bin_stamp)
  test('fuse-anchor', sh, args : files('../fuse/test_fuse_anchor.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 900, depends : bin_stamp)
  test('fuse-guard', sh, args : files('../fuse/test_fuse_guard.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-freeze', sh, args : files('../fuse/test_fuse_freeze.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-freeze-anchor', sh, args : files('../fuse/test_fuse_freeze_anchor.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-rwyctl', sh, args : files('../fuse/test_fuse_rwyctl.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-precopy', sh, args : files('../fuse/test_fuse_precopy.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 300, depends : bin_stamp)
  test('fuse-blobs-enc', sh, args : files('../fuse/test_fuse_blobs_enc.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 300, depends : bin_stamp)
  test('fuse-rwyctl-cases', sh, args : files('../fuse/test_fuse_rwyctl_cases.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-rwyctl-anchor', sh, args : files('../fuse/test_fuse_rwyctl_anchor.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-content', sh, args : files('../fuse/test_fuse_content.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-view', sh, args : files('../fuse/test_fuse_view.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 300, depends : bin_stamp)
  test('fuse-bridge-blobs', sh, args : files('../fuse/test_fuse_bridge_blobs.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-loop', sh, args : files('../fuse/test_fuse_loop.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
  test('fuse-buffer', sh, args : files('../fuse/test_fuse_buffer.sh'), env : host_env,
    workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 120, depends : bin_stamp)
endif

# Needs A64_BUILD, the tests/host/bin of a -Dnative_tools=false arm64 cross build; skips without it.
if not meson.is_cross_build()
  test('xarch', sh, args : files('test_xarch.sh'), env : host_env,
    workdir : src_root, suite : 'host', timeout : 900, depends : bin_stamp)
endif

# The scratch directories of other tests would show up in its before and after lists.
test('tmpdirs', sh, args : files('test_tmpdirs.sh'), env : host_env,
  workdir : src_root, suite : 'host', is_parallel : false, timeout : 900, depends : bin_stamp)

soak_env = host_env
soak_env += {'SOAK_CYCLES' : get_option('soak_cycles').to_string()}
test('rotation-soak', sh, args : files('test_rotation.sh'), env : soak_env,
  workdir : src_root, suite : 'soak', is_parallel : false, timeout : 0, depends : bin_stamp)

# The ARMv8 AES code only exists in the arm64 build; it runs under qemu-aarch64 (the exe_wrapper).
if arch == 'aarch64'
  aesarmtest = executable('aesarmtest',
    files('aesarmtest.c', '../../src/rwy_aes.c', '../../src/rwy_xts.c', '../../src/rwy_crypto.c',
          '../../src/rwy_sha256.c'),
    include_directories : [host_inc, src_inc], link_args : '-static')
  test('aesarmtest', aesarmtest, suite : 'host')
endif
