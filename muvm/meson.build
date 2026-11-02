# SPDX-License-Identifier: GPL-2.0-only
executable(
  'mangohud-muvm',
  'mangohud-muvm.cpp',
  install: true,
)

muvm_config = configuration_data()
muvm_config.set('MUVM_EXECUTABLE', get_option('prefix') / get_option('bindir') / 'mangohud-muvm')

unit_dir = get_option('datadir') / 'systemd/user'

configure_file(
  input: 'mangohud-muvm.service.in',
  output: 'mangohud-muvm.service',
  configuration: muvm_config,
  install_dir: unit_dir,
)

# Started by a guest HUD's request only, so it is on for every user.
install_data('mangohud-muvm.path', install_dir: unit_dir)
install_symlink(
  'mangohud-muvm.path',
  pointing_to: '..' / 'mangohud-muvm.path',
  install_dir: unit_dir / 'default.target.wants',
)
