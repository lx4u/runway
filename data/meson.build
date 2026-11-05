# SPDX-License-Identifier: Apache-2.0
systemd_unit_dir = get_option('systemd_unit_dir')
if systemd_unit_dir == ''
  systemd = dependency('systemd', required : false)
  if systemd.found()
    systemd_unit_dir = systemd.get_variable('systemdsystemunitdir')
  else
    systemd_unit_dir = get_option('prefix') / 'lib/systemd/system'
  endif
endif

install_data('systemd/boot.mount',
  install_dir : systemd_unit_dir)

install_data('systemd/systemd-bless-boot.service.d/10-runway.conf',
  install_dir : systemd_unit_dir / 'systemd-bless-boot.service.d')

install_data('kernel/install.conf.d/10-runway.conf',
  install_dir : get_option('prefix') / 'lib/kernel/install.conf.d')

install_data('profile.d/10-runway.sh',
  install_dir : get_option('sysconfdir') / 'profile.d')
