# SPDX-License-Identifier: Apache-2.0
efi_c_args = [
  '-DRWY_EFI',
  '-ffreestanding', '-fno-strict-aliasing', '-fno-stack-protector',
  '-fshort-wchar', '-fpic',
  '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
  '-fno-asynchronous-unwind-tables',
]
if arch == 'x86_64'
  efi_c_args += ['-DHAVE_USE_MS_ABI', '-mno-red-zone']
endif
efi_inc = include_directories(gnuefi_inc, gnuefi_inc / arch, '../include', '../src')

fs = import('fs')
gnuefi_lib_dir = gnuefi_lib
if fs.is_file(gnuefi_lib / (arch + '-linux-gnu') / 'crt0-efi-@0@.o'.format(arch))
  gnuefi_lib_dir = gnuefi_lib / (arch + '-linux-gnu')
endif

efi_link_args = [
  '-nostdlib', '-shared', '-Wl,-Bsymbolic', '-Wl,-znocombreloc', '-Wl,--no-undefined',
  '-Wl,-z,norelro',
  '-Wl,-T,' + gnuefi_lib_dir / 'elf_@0@_efi.lds'.format(arch),
  gnuefi_lib_dir / 'crt0-efi-@0@.o'.format(arch),
  '-L' + gnuefi_lib_dir, '-lefi', '-lgnuefi', '-lgcc',
]
if arch == 'aarch64'
  efi_link_args += ['-Wl,--defsym=EFI_SUBSYSTEM=0xa']
endif

objcopy = find_program('objcopy')
check_undef = find_program('../tools/check_undef.sh')
check_stub = find_program('../tools/check_stub.sh')

# -j .rodata is essential: string literals live there.
objcopy_args = []
foreach s : ['.text', '.sdata', '.data', '.dynamic', '.dynsym', '.rel', '.rela', '.reloc', '.rodata']
  objcopy_args += ['-j', s]
endforeach

efi_util_src = files('../src/efi_util.c')

# The file system adapter and the two backends: what the benchmark mounts a store with.
fs_src = files('../src/rwy_fs.c', '../src/rwy_fs_path.c', '../src/rwy_fs_info.c', '../src/rwy_filebe.c',
  '../src/rwy_blockio.c')

stub_src = files(
  '../src/rwy_stub_main.c',
  '../src/rwy_loader_fsm.c',
  '../src/rwy_bundle.c',
  '../src/rwy_partscan.c',
  '../src/rwy_gpt.c',
  '../src/rwy_partinfo.c',
  '../src/rwy_blockio.c',
  '../src/rwy_filebe.c',
  '../src/rwy_util.c',
  '../src/rwy_sha256.c',
  '../src/rwy_io.c')

rwy_src = core_src + crypto_src + files(
  '../src/rwy_main.c', '../src/rwy_dbglog.c', '../src/rwy_diag.c', '../src/rwy_partinfo.c', '../src/rwy_gpt.c',
  '../src/rwy_partscan.c', '../src/rwy_scan.c', '../src/rwy_resync.c', '../src/rwy_unlock.c',
  '../src/rwy_grpmount.c', '../src/rwy_efianchor.c', '../src/rwy_slotboot.c',
  '../src/rwy_bootvars.c', '../src/rwy_guardvar.c', '../src/rwy_loadervars.c', '../src/rwy_handover.c', '../src/rwy_recboot.c',
  '../src/rwy_bundle.c', '../src/rwy_loader_fsm.c', '../src/rwy_recarea.c', '../src/rwy_wdt.c', '../src/rwy_fs.c', '../src/rwy_fs_path.c',
  '../src/rwy_fs_info.c', '../src/rwy_filebe.c', '../src/rwy_blockio.c', '../src/rwy_tpm.c', '../src/rwy_tpm_cmd.c',
  '../src/rwy_netreport.c')

# Every application is the sources below plus efi_util. CHAIN.EFI is the consumer of the
# unmodified protocol: it must not link the Runway core.
efi_apps = {
  'CHAIN' : files('../src/chain_main.c'),
  'NATIVE_PAYLOAD' : files('../tests/efi/native_payload.c'),
  'SDBOOT_PAYLOAD' : files('../tests/efi/sdboot_payload.c'),
  'REINSTALL_PROBE' : files('../tests/efi/reinstall_probe.c'),
  'REINSTALL_CHILD' : files('../tests/efi/reinstall_child.c'),
  'IOBENCH' : files('../tests/efi/iobench.c', '../tests/efi/iobench_rwy.c', '../tests/efi/iobench_csv.c',
                    '../tests/efi/iobench_config.c', '../tests/efi/iobench_result.c',
                    '../tests/efi/iobench_work.c', '../tests/efi/iobench_stats.c',
                    '../tests/efi/iobench_cipher.c', '../tests/efi/iobench_wpath.c',
                    '../tests/efi/iobench_probe.c') + core_src + crypto_src + fs_src,
  'BLOCKIO_TEST' : files('../tests/efi/blockio_test.c', '../src/rwy_blockio.c'),
  'PARTINFO_TEST' : files('../tests/efi/partinfo_test.c', '../src/rwy_partinfo.c', '../src/rwy_gpt.c'),
  'FILEBE_TEST' : files('../tests/efi/filebe_test.c', '../src/rwy_filebe.c'),
  'I6300ESB' : files('../drivers/wdt/i6300esb.c', '../drivers/wdt/wdt_common.c'),
  'WDT_RUNNER' : files('../tests/efi/wdt_runner.c'),
  'WDT_STUB_FAIL' : files('../tests/efi/wdt_stub_fail.c'),
  'WDT_STUB_UNSUP' : files('../tests/efi/wdt_stub_unsup.c'),
  'WDT_STUB_OK' : files('../tests/efi/wdt_stub_ok.c'),
  'LOADER_FAIL' : files('../tests/efi/loader_fail.c'),
  'LOADER_HANG' : files('../tests/efi/loader_hang.c'),
  'LOADER_RESET' : files('../tests/efi/loader_reset.c'),
  'RUNWAY' : stub_src,
  'rwy-loader' : rwy_src,
  'RUNWAY_DEBUG' : rwy_src,
}

if arch == 'x86_64'
  efi_apps += {
    'ITCO' : files('../drivers/wdt/itco.c', '../drivers/wdt/wdt_common.c'),
    'WDAT' : files('../drivers/wdt/wdat.c', '../drivers/wdt/wdt_common.c'),
  }
endif

efi = {}
efi_list = []
foreach name, src : efi_apps
  # RUNWAY_DEBUG.EFI has the RWY_DEBUG log lines the mirror and TPM suites read.
  args = efi_c_args
  if name == 'RUNWAY_DEBUG'
    args += '-DRWY_DEBUG_LOG'
  endif
  so = executable(name, src, efi_util_src,
    c_args : args, include_directories : efi_inc, link_args : efi_link_args,
    name_suffix : 'so')
  undef = custom_target(name + '.undef',
    input : so, output : name + '.undef',
    command : [check_undef, '@INPUT@', '@OUTPUT@'])
  is_wdt = (name == 'I6300ESB' or name == 'ITCO' or name == 'WDAT')
  is_loader = (name == 'RUNWAY' or name == 'rwy-loader')
  inst_dir = is_wdt ? (get_option('libdir') / 'runway' / 'wdt') : (get_option('libdir') / 'runway')
  out_name = (name == 'rwy-loader') ? (name + '.efi') : (name + '.EFI')
  efi_app = custom_target(out_name,
    input : so, output : out_name, depends : undef,
    command : [objcopy, objcopy_args, '--output-target=efi-app-' + arch, '@INPUT@', '@OUTPUT@'],
    install : (is_loader or is_wdt),
    install_dir : inst_dir,
    install_mode : 'rw-r--r--',
    build_by_default : true)
  if name == 'RUNWAY'
    custom_target('check_stub',
      input : [efi_app, so], output : 'stub.checked',
      command : [check_stub, '@INPUT0@', '@INPUT1@', '@OUTPUT@'],
      build_by_default : true)
  endif
  efi += {name : efi_app}
  efi_list += efi_app
endforeach

chain_efi = efi['CHAIN']
