# SPDX-License-Identifier: Apache-2.0
mkbin = find_program('../mkbin.sh')

# One directory per EFI variant with every program and file the scripts take from $BUILD.
qemu_inputs = [fsck_runway, rwyctl, mkstore, host_fs, host_mutate, host_cat, iobench_linux, seed_store, tpmtest] + efi_list
if fuse3.found()
  qemu_inputs += rwy_fuse
endif

qemu_bin = custom_target('qemu-bin',
  input : qemu_inputs, output : 'qemu-bin.stamp',
  command : [mkbin, '@OUTDIR@/bin', '@OUTPUT@', '@INPUT@'],
  build_by_default : true)
qemu_bin_debug = custom_target('qemu-bin-debug',
  input : qemu_inputs, output : 'qemu-bin-debug.stamp',
  command : [mkbin, '-d', '@OUTDIR@/bin-debug', '@OUTPUT@', '@INPUT@'],
  build_by_default : true)

qemu_env = {
  'OVMF_CODE' : get_option('ovmf_code'),
  'OVMF_VARS_TEMPLATE' : get_option('ovmf_vars'),
  'ARCH' : arch,
  'STORE' : 'block',   # the store layout of the test images, see tools/imglib.sh
}
qemu_bin_dir = meson.current_build_dir() / 'bin'
qemu_env_normal = qemu_env + {'BUILD' : qemu_bin_dir}
qemu_env_debug = qemu_env + {'BUILD' : meson.current_build_dir() / 'bin-debug'}

# The mirror, multi-volume, TPM and A/B suites read the RUNWAY_DEBUG lines of RUNWAY.EFI.
# The scripts share image files, so they run one at a time.
qemu_tests = [
  ['native', 'qemu', 'test_native.sh', false],
  ['reinstall', 'qemu', 'test_reinstall.sh', false],
  ['iobench', 'qemu', 'test_iobench.sh', false],
  ['iobench-image', 'qemu', 'test_iobench_image.sh', false],
  ['iobench-disks', 'qemu', 'test_iobench_disks.sh', false],
  ['iobench-pause', 'qemu', 'test_iobench_pause.sh', false],
  ['iobench-probe', 'qemu', 'test_iobench_probe.sh', false],
  ['iobench-provision', 'qemu', 'test_iobench_provision.sh', false],
  ['blockio', 'qemu', 'test_blockio.sh', false],
  ['partinfo', 'qemu', 'test_partinfo.sh', false],
  ['dbglog', 'qemu', 'test_dbglog.sh', true],
  ['block', 'qemu', 'test_block.sh', true],
  ['tpm', 'qemu', 'test_tpm.sh', true],
  ['tpm-multi', 'qemu', 'test_tpm_multi.sh', true],
  ['mirror', 'qemu', 'test_mirror.sh', true],
  ['multivol', 'qemu', 'test_multivol.sh', true],
  ['sdboot', 'qemu', 'test_sdboot.sh', true],
  ['loopguard', 'qemu', 'test_loopguard.sh', false],
  ['wdt', 'wdt', 'test_wdt.sh', false],
  ['wdt-boot', 'wdt', 'test_wdt_boot.sh', false],
  ['loader', 'qemu', 'test_loader.sh', false],
  ['netreport', 'qemu', 'test_netreport.sh', false],
]
foreach t : qemu_tests
  debug = t[3]
  test(t[0], sh, args : files(t[2]),
    env : debug ? qemu_env_debug : qemu_env_normal,
    depends : debug ? qemu_bin_debug : qemu_bin,
    workdir : src_root, suite : t[1], is_parallel : false, timeout : 3600)
endforeach

# A prepared disk whose encrypted store has another key is provisioned again.
test('iobench-provision-stale', sh, args : files('test_iobench_provision.sh'),
  env : qemu_env_normal + {'STALE' : '1'}, depends : qemu_bin,
  workdir : src_root, suite : 'qemu', is_parallel : false, timeout : 3600)

# The same flow with a TPM 2.0: the loader must not touch the benchmark stores.
test('iobench-provision-tpm', sh, args : files('test_iobench_provision.sh'),
  env : qemu_env_normal + {'TPM' : '1'}, depends : qemu_bin,
  workdir : src_root, suite : 'qemu', is_parallel : false, timeout : 3600)

# The same flow with the boot disk on USB and the target on NVMe, as on a PC.
test('iobench-provision-usb-nvme', sh, args : files('test_iobench_provision.sh'),
  env : qemu_env_normal + {'TOPOLOGY' : 'usb-nvme'}, depends : qemu_bin,
  workdir : src_root, suite : 'qemu', is_parallel : false, timeout : 3600)

# The file backend keeps a small suite of its own, so that it does not rot.
qemu_file_tests = [
  ['native-file', 'test_native.sh', false],
  ['reinstall-file', 'test_reinstall.sh', false],
  ['mirror-file', 'test_mirror.sh', true],
  ['filebe', 'test_filebe.sh', false],
]
foreach t : qemu_file_tests
  debug = t[2]
  test(t[0], sh, args : files(t[1]),
    env : (debug ? qemu_env_debug : qemu_env_normal) + {'STORE' : 'file'},
    depends : debug ? qemu_bin_debug : qemu_bin,
    workdir : src_root, suite : 'qemu-file', is_parallel : false, timeout : 3600)
endforeach

if fuse3.found()
  test('fuse-index', sh, args : files('../fuse/test_fuse_index.sh'), env : qemu_env_normal,
    depends : qemu_bin, workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 600)
  test('fuse-bls', sh, args : files('../fuse/test_fuse_bls.sh'), env : qemu_env_normal,
    depends : qemu_bin, workdir : src_root, suite : 'fuse', is_parallel : false, timeout : 600)
endif

if not get_option('debian_guest').disabled()
  test('debian', sh, args : files('test_debian.sh'), env : qemu_env_normal,
    depends : qemu_bin, workdir : src_root, suite : 'qemu', is_parallel : false, timeout : 1800)
  test('debian-file', sh, args : files('test_debian.sh'), env : qemu_env_normal + {'STORE' : 'file'},
    depends : qemu_bin, workdir : src_root, suite : 'qemu-file', is_parallel : false, timeout : 1800)
  test('luksroot', sh, args : files('test_luksroot.sh'), env : qemu_env_normal,
    depends : qemu_bin, workdir : src_root, suite : 'qemu', is_parallel : false, timeout : 1800)
endif


# Developer runs, from build/bin: meson compile run | run-debug | run-mirror | run-tpm | run-tpm-debug
run_qemu = files('../../tools/run_qemu.sh')
foreach mode : ['run', 'run-debug', 'run-mirror', 'run-tpm', 'run-tpm-debug']
  run_target(mode, command : [sh, run_qemu, mode], env : qemu_env_normal, depends : qemu_bin)
endforeach
