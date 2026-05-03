project(
  'uniOS',
  ['c', 'cpp', 'nasm'],
  version : '0.1.0',
  meson_version : '>=1.4.0',
  default_options : [
    'buildtype=release',
    'cpp_std=c++20',
    'warning_level=3',
  ],
)

py = import('python').find_installation('python3')
fs = import('fs')
cpp = meson.get_compiler('cpp')
ld_lld = find_program('ld.lld', required : true)
lld_link = find_program('lld-link', required : true)
clangxx_bin = find_program('clang++', required : true)

build_root = meson.project_build_root()
if cpp.get_id() != 'clang'
  error('uniOS requires the LLVM toolchain. Configure with `meson setup builddir --cross-file toolchains/llvm.ini`.')
endif
if not meson.is_cross_build()
  error('uniOS must be configured as a cross build. Use `meson setup builddir --cross-file toolchains/llvm.ini`.')
endif

list_sources = files('tools/meson_list_sources.py')[0]
stage_rootfs_script = files('tools/stage_rootfs.py')[0]
mkunifs = files('tools/mkunifs.py')[0]
create_iso = files('tools/create_iso.py')[0]
build_efi_image = files('tools/build_efi_image.py')[0]
qemu_smoke = files('tools/qemu_smoke.py')[0]
wallpaper_package = files('tools/wallpaper_package.py')[0]
linker_script = files('linker.ld')[0]
user_linker_script = files('src/usr/user.ld')[0]

kernel_source_output = run_command(
  py,
  list_sources,
  'src',
  '--glob', '*.cpp',
  '--glob', '*.asm',
  '--exclude-prefix', 'src/usr',
  '--exclude-prefix', 'src/bootloader',
  check : true,
).stdout().strip()
kernel_source_paths = []
if kernel_source_output != ''
  kernel_source_paths = kernel_source_output.split('\n')
endif
# Existing build directories do not rerun the directory scan just because a new source
# file appears, so keep newly added kernel translation units visible to Meson regeneration.
if not kernel_source_paths.contains('src/drivers/storage/usb_msc.cpp')
  kernel_source_paths += ['src/drivers/storage/usb_msc.cpp']
endif
kernel_sources = files(kernel_source_paths)

kernel_inc = include_directories('include', 'src')
user_inc = include_directories('include', 'src/usr/libc')

git_commit = run_command(
  'git',
  'rev-parse',
  '--short',
  'HEAD',
  check : false,
).stdout().strip()
if git_commit == ''
  git_commit = 'unknown'
endif

buildtype = get_option('buildtype')
build_is_debug = buildtype == 'debug' or buildtype == 'debugoptimized'

kernel_common_args = [
  '-ffreestanding',
  '-fno-exceptions',
  '-fno-rtti',
  '-fno-stack-protector',
  '-fcf-protection=none',
  '-mcmodel=kernel',
  '-mno-red-zone',
  '-march=x86-64',
  '-mtune=generic',
  '-fno-strict-aliasing',
  '-fno-delete-null-pointer-checks',
  '-fno-common',
  '-fno-omit-frame-pointer',
  '-fno-pie',
  '-ffunction-sections',
  '-fdata-sections',
  '-Wall',
  '-Wextra',
  '-Wpedantic',
  '-Wconversion',
  '-Wsign-conversion',
  '-Wformat=2',
  '-Wshadow',
]

freestanding_warning_args = [
  '-Wno-address-of-packed-member',
  '-Wno-comment',
  '-Wno-conversion',
  '-Wno-deprecated-volatile',
  '-Wno-missing-field-initializers',
  '-Wno-sign-conversion',
  '-Wno-unused-const-variable',
  '-Wno-unused-function',
  '-Wno-unused-variable',
]

kernel_common_args += freestanding_warning_args + [
  '-DGIT_COMMIT="@0@"'.format(git_commit),
]

kernel_build_args = ['-O2']
if buildtype == 'debug'
  kernel_build_args = ['-g', '-O0']
elif buildtype == 'debugoptimized'
  kernel_build_args = ['-g', '-O2']
endif
if build_is_debug
  kernel_build_args += ['-DDEBUG']
else
  kernel_build_args += ['-DNDEBUG']
endif

user_build_args = ['-O2']
if buildtype == 'debug'
  user_build_args = ['-g', '-O0']
elif buildtype == 'debugoptimized'
  user_build_args = ['-g', '-O2']
endif
if build_is_debug
  user_build_args += ['-DDEBUG']
else
  user_build_args += ['-DNDEBUG']
endif

boot_nasm_args = ['-f', 'bin']
if build_is_debug
  boot_nasm_args += ['-DDEBUG']
endif

kernel_archive = static_library(
  'kernel_objs',
  kernel_sources,
  include_directories : kernel_inc,
  cpp_args : kernel_common_args + kernel_build_args,
  nasm_args : ['-f', 'elf64'],
)

kernel = custom_target(
  'kernel',
  input : [kernel_archive, linker_script],
  output : 'kernel.elf',
  command : [
    ld_lld,
    '-m',
    'elf_x86_64',
    '-o',
    '@OUTPUT@',
    '-T',
    linker_script,
    '-z',
    'max-page-size=0x1000',
    '-z',
    'noexecstack',
    '-z',
    'relro',
    '-z',
    'now',
    '--gc-sections',
    '--whole-archive',
    kernel_archive,
    '--no-whole-archive',
  ],
)

user_c_args = [
  '-std=c11',
  '-ffreestanding',
  '-fno-stack-protector',
  '-fno-pie',
  '-mno-red-zone',
  '-fcf-protection=none',
  '-Wall',
  '-Wextra',
  '-DGIT_COMMIT="@0@"'.format(git_commit),
] + freestanding_warning_args + user_build_args

user_cpp_args = [
  '-ffreestanding',
  '-fno-exceptions',
  '-fno-rtti',
  '-fno-stack-protector',
  '-fno-pie',
  '-fcf-protection=none',
  '-mno-red-zone',
  '-Wall',
  '-Wextra',
  '-DGIT_COMMIT="@0@"'.format(git_commit),
] + freestanding_warning_args + user_build_args

crt0 = static_library(
  'crt0',
  'src/usr/libc/crt0.asm',
  nasm_args : ['-f', 'elf64'],
)

libc = static_library(
  'uni_libc',
  [
    'src/usr/libc/cxx.cpp',
    'src/usr/libc/unistd.c',
    'src/usr/libc/stdlib.c',
    'src/usr/libc/string.c',
    'src/usr/libc/stdio.c',
    'src/usr/libc/socket.c',
    'src/usr/libc/wav.c',
    'src/usr/libc/log.c',
  ],
  include_directories : user_inc,
  c_args : user_c_args,
  cpp_args : user_cpp_args,
)

libgui = static_library(
  'uni_libgui',
  [
    'src/usr/libgui/gui.cpp',
    'src/usr/libgui/font.cpp',
    'src/usr/libgui/image.cpp',
  ],
  include_directories : [kernel_inc, user_inc],
  cpp_args : user_cpp_args,
)

app_sources = {
  'shell' : ['src/usr/shell'],
  'init' : ['src/usr/init'],
  'wm' : ['src/usr/wm'],
  'menubar' : ['src/usr/menubar'],
  'dock' : ['src/usr/dock'],
  'files' : ['src/usr/apps/files'],
  'terminal' : ['src/usr/apps/terminal'],
  'latitude' : ['src/usr/apps/latitude'],
  'about' : ['src/usr/apps/about'],
  'preferences' : ['src/usr/apps/preferences'],
}

user_apps = {}
user_app_targets = []
foreach app_name, search_roots : app_sources
  output = run_command(
    py,
    list_sources,
    search_roots[0],
    '--glob', '*.cpp',
    check : true,
  ).stdout().strip()
  source_paths = []
  if output != ''
    source_paths = output.split('\n')
  endif
  app_files = files(source_paths)
  app_link_with = [crt0, libc]
  if app_name != 'shell' and app_name != 'init'
    app_link_with += [libgui]
  endif
  app_archive = static_library(
    '@0@_objs'.format(app_name),
    app_files,
    include_directories : [kernel_inc, user_inc],
    cpp_args : user_cpp_args,
  )

  app_link_command = [
    ld_lld,
    '-m',
    'elf_x86_64',
    '-o',
    '@OUTPUT@',
    '-T',
    user_linker_script,
    '--whole-archive',
  ]
  foreach link_input : app_link_with
    app_link_command += [link_input]
  endforeach
  app_link_command += [
    app_archive,
    '--no-whole-archive',
  ]

  app_target = custom_target(
    app_name,
    input : [user_linker_script, app_archive] + app_link_with,
    output : '@0@.elf'.format(app_name),
    command : app_link_command,
  )
  user_apps += {app_name : app_target}
  user_app_targets += [app_target]
endforeach

stage_overlays = []

foreach app_name, app_target : user_apps
  stage_overlays += ['--overlay', 'bin/@0@.elf'.format(app_name), app_target]
endforeach

default_wallpaper = custom_target(
  'default-wallpaper',
  input : [
    wallpaper_package,
    files('assets/wallpapers/wp_light.svg')[0],
    files('assets/wallpapers/wp_dark.svg')[0],
  ],
  output : 'default.uowp',
  command : [
    py,
    wallpaper_package,
    '--light',
    files('assets/wallpapers/wp_light.svg')[0],
    '--dark',
    files('assets/wallpapers/wp_dark.svg')[0],
    '--output',
    '@OUTPUT@',
  ],
)

stage_overlays += ['--overlay', 'usr/share/wallpapers/default.uowp', default_wallpaper]

rootfs_stamp = custom_target(
  'stage-rootfs',
  input : user_app_targets + [default_wallpaper],
  output : 'rootfs.stamp',
  command : [
    py,
    stage_rootfs_script,
    '--source-rootfs',
    meson.project_source_root() / 'rootfs',
    '--build-rootfs',
    build_root / 'rootfs',
    '--stamp',
    '@OUTPUT@',
  ] + stage_overlays,
  build_always_stale : true,
)

unifs = custom_target(
  'unifs',
  input : [mkunifs, rootfs_stamp],
  output : 'unifs.img',
  command : [py, mkunifs, build_root / 'rootfs', '@OUTPUT@'],
)

nasm_bin = find_program('nasm', required : true)
uefi_build_args = [
  '-target', 'x86_64-pc-windows-msvc',
  '-std=c++20',
  '-ffreestanding',
  '-fshort-wchar',
  '-fno-exceptions',
  '-fno-rtti',
  '-fno-stack-protector',
  '-fno-builtin',
  '-fcf-protection=none',
  '-mno-red-zone',
  '-Wall',
  '-Wextra',
  '-Wpedantic',
  '-Wconversion',
  '-Wsign-conversion',
  '-DGIT_COMMIT="@0@"'.format(git_commit),
]
if buildtype == 'debug'
  uefi_build_args += ['-g', '-O0']
elif buildtype == 'debugoptimized'
  uefi_build_args += ['-g', '-O2']
else
  uefi_build_args += ['-O2']
endif
if build_is_debug
  uefi_build_args += ['-DDEBUG']
else
  uefi_build_args += ['-DNDEBUG']
endif

uefi_main_obj = custom_target(
  'uefi-main-obj',
  input : files('src/bootloader/uefi/main.cpp'),
  output : 'uefi_main.obj',
  command : [
    clangxx_bin,
  ] + uefi_build_args + [
    '-I', meson.project_source_root() / 'include',
    '-I', meson.project_source_root() / 'src',
    '-c',
    '@INPUT@',
    '-o',
    '@OUTPUT@',
  ],
)

uefi_handoff_obj = custom_target(
  'uefi-handoff-obj',
  input : files('src/bootloader/uefi/handoff.asm'),
  output : 'uefi_handoff.obj',
  command : [nasm_bin, '-f', 'win64', '@INPUT@', '-o', '@OUTPUT@'],
)

uefi_bootloader = custom_target(
  'uefi-bootloader',
  input : [uefi_main_obj, uefi_handoff_obj],
  output : 'BOOTX64.EFI',
  command : [
    lld_link,
    '/subsystem:efi_application',
    '/entry:efi_main',
    '/machine:x64',
    '/nodefaultlib',
    '/opt:ref',
    '/opt:icf',
    '/out:@OUTPUT@',
    '@INPUT@',
  ],
)

efi_system_image = custom_target(
  'efi-system-image',
  input : [build_efi_image, uefi_bootloader, kernel, unifs],
  output : 'efi.img',
  command : [
    py,
    build_efi_image,
    '--mode',
    'fat',
    '--bootloader',
    uefi_bootloader,
    '--kernel',
    kernel,
    '--unifs',
    unifs,
    '@OUTPUT@',
  ],
)

boot_disk = custom_target(
  'boot-disk',
  input : [build_efi_image, efi_system_image],
  output : 'boot.img',
  command : [
    py,
    build_efi_image,
    '--mode',
    'disk',
    '--efi-image',
    efi_system_image,
    '@OUTPUT@',
  ],
)

iso_image = custom_target(
  'iso-image',
  input : [create_iso, efi_system_image, uefi_bootloader, kernel, unifs],
  output : 'uniOS.iso',
  command : [
    py,
    create_iso,
    '--efi-image',
    efi_system_image,
    '--bootloader',
    uefi_bootloader,
    '--kernel',
    kernel,
    '--unifs',
    unifs,
    '@OUTPUT@',
  ],
)

alias_target('iso', iso_image)
alias_target('image', boot_disk)
alias_target('disk-image', boot_disk)
alias_target('usb-image', boot_disk)
alias_target('efi-image', efi_system_image)

qemu = find_program('qemu-system-x86_64', required : false)
ovmf_code_candidates = [
  'C:\\Program Files\\qemu\\share\\edk2-x86_64-code.fd',
  '/usr/share/OVMF/OVMF_CODE.fd',
  '/usr/share/OVMF/OVMF_CODE_4M.fd',
  '/usr/share/edk2/ovmf/OVMF_CODE.fd',
  '/usr/share/edk2/x64/OVMF_CODE.fd',
]
ovmf_code = ''
foreach candidate : ovmf_code_candidates
  if ovmf_code == '' and fs.exists(candidate)
    ovmf_code = candidate
  endif
endforeach
if qemu.found() and ovmf_code != '' and fs.exists(ovmf_code)
  qemu_path = qemu.full_path()
  qemu_machine = 'q35'
  if build_machine.system() == 'windows'
    qemu_machine = 'pc'
  endif

  qemu_base_command = [
    qemu_path,
    '-machine',
    qemu_machine,
    '-drive',
    'if=pflash,format=raw,readonly=on,file=@0@'.format(ovmf_code),
    '-m',
    '512M',
  ]

  qemu_disk_boot_command = [
    '-drive',
    'file=@0@,format=raw,if=ide,index=0'.format(boot_disk.full_path()),
  ]
  qemu_usb_boot_command = [
    '-drive',
    'file=@0@,format=raw,if=none,id=usbstick'.format(boot_disk.full_path()),
    '-device',
    'usb-storage,drive=usbstick,bootindex=0',
  ]
  qemu_esp_usb_boot_command = [
    '-drive',
    'file=@0@,format=raw,if=none,id=esp'.format(efi_system_image.full_path()),
    '-device',
    'usb-storage,drive=esp,bootindex=0',
  ]
  qemu_usb_devices = [
    '-device',
    'qemu-xhci,id=xhci',
    '-device',
    'usb-kbd,bus=xhci.0',
    '-device',
    'usb-mouse,bus=xhci.0',
  ]
  if build_machine.system() == 'windows'
    qemu_usb_devices = [
      '-usb',
      '-device',
      'usb-kbd',
      '-device',
      'usb-mouse',
    ]
  endif

  qemu_net_devices = [
    '-netdev',
    'user,id=net0',
    '-device',
    'e1000,netdev=net0',
  ]
  qemu_serial_stdio = ['-serial', 'stdio']
  qemu_headless = ['-serial', 'stdio', '-display', 'none']
  qemu_no_reboot = ['-no-reboot']

  run_target(
    'run-qemu',
    depends : [boot_disk],
    command : qemu_base_command + qemu_disk_boot_command,
  )
  run_target(
    'run-qemu-serial',
    depends : [boot_disk],
    command : qemu_base_command + qemu_disk_boot_command + qemu_serial_stdio,
  )
  run_target(
    'run-qemu-headless',
    depends : [boot_disk],
    command : qemu_base_command + qemu_disk_boot_command + qemu_headless,
  )
  run_target(
    'run-qemu-usb',
    depends : [boot_disk],
    command : qemu_base_command + qemu_usb_devices + qemu_disk_boot_command,
  )
  run_target(
    'run-qemu-usb-serial',
    depends : [boot_disk],
    command : qemu_base_command + qemu_usb_devices + qemu_disk_boot_command + qemu_serial_stdio,
  )
  run_target(
    'run-qemu-net',
    depends : [boot_disk],
    command : qemu_base_command + qemu_net_devices + qemu_disk_boot_command,
  )
  run_target(
    'run-qemu-net-serial',
    depends : [boot_disk],
    command : qemu_base_command + qemu_net_devices + qemu_disk_boot_command + qemu_serial_stdio,
  )
  run_target(
    'run-qemu-full',
    depends : [boot_disk],
    command : qemu_base_command + qemu_usb_devices + qemu_net_devices + qemu_disk_boot_command + qemu_serial_stdio,
  )
  run_target(
    'run-qemu-esp-usb',
    depends : [efi_system_image],
    command : qemu_base_command + qemu_usb_devices + qemu_esp_usb_boot_command,
  )
  run_target(
    'run-qemu-esp-usb-serial',
    depends : [efi_system_image],
    command : qemu_base_command + qemu_usb_devices + qemu_esp_usb_boot_command + qemu_serial_stdio,
  )
  run_target(
    'run-qemu-esp-usb-headless',
    depends : [efi_system_image],
    command : qemu_base_command + qemu_usb_devices + qemu_esp_usb_boot_command + qemu_headless,
  )

  run_target('run', depends : [boot_disk], command : qemu_base_command + qemu_disk_boot_command)
  run_target('run-serial', depends : [boot_disk], command : qemu_base_command + qemu_disk_boot_command + qemu_serial_stdio)
  run_target('run-headless', depends : [boot_disk], command : qemu_base_command + qemu_disk_boot_command + qemu_headless)
  run_target('run-disk', depends : [boot_disk], command : qemu_base_command + qemu_disk_boot_command)
  run_target('run-disk-serial', depends : [boot_disk], command : qemu_base_command + qemu_disk_boot_command + qemu_serial_stdio)
  run_target('run-disk-headless', depends : [boot_disk], command : qemu_base_command + qemu_disk_boot_command + qemu_headless)
  run_target('run-usb', depends : [boot_disk], command : qemu_base_command + qemu_usb_devices + qemu_usb_boot_command)
  run_target('run-usb-serial', depends : [boot_disk], command : qemu_base_command + qemu_usb_devices + qemu_usb_boot_command + qemu_serial_stdio)
  run_target('run-usb-headless', depends : [boot_disk], command : qemu_base_command + qemu_usb_devices + qemu_usb_boot_command + qemu_headless)

  qemu_smoke_success_args = [
    '--success',
    'first desktop frame submitted',
  ]
  if build_is_debug
    qemu_smoke_success_args += [
      '--success',
      'ktest suite passed',
    ]
  endif

  test(
    'qemu-smoke',
    py,
    args : [
      qemu_smoke,
      '--timeout',
      '30',
    ] + qemu_smoke_success_args + [
      '--fail',
      'ktest suite failed',
      '--fail',
      'KERNEL PANIC',
      '--',
    ] + qemu_base_command + qemu_disk_boot_command + qemu_headless + qemu_no_reboot,
    depends : [boot_disk],
    suite : 'smoke',
    timeout : 45,
  )
endif

clang_format = find_program('clang-format', required : false)
if clang_format.found()
  format_files_output = run_command(
    py,
    list_sources,
    'src',
    '--glob', '*.cpp',
    '--glob', '*.h',
    check : true,
  ).stdout().strip()
  include_files_output = run_command(
    py,
    list_sources,
    'include',
    '--glob', '*.h',
    check : true,
  ).stdout().strip()
  format_files = []
  if format_files_output != ''
    foreach source_file : format_files_output.split('\n')
      format_files += [meson.project_source_root() / source_file]
    endforeach
  endif
  if include_files_output != ''
    foreach include_file : include_files_output.split('\n')
      format_files += [meson.project_source_root() / include_file]
    endforeach
  endif
  if format_files.length() > 0
    run_target('format', command : [clang_format, '-i'] + format_files)
  endif
endif

cppcheck = find_program('cppcheck', required : false)
if cppcheck.found()
  run_target(
    'lint',
    command : [cppcheck, '--enable=all', '--suppress=missingIncludeSystem', '--error-exitcode=1', '-D__asm__(x)=', meson.project_source_root() / 'src', meson.project_source_root() / 'include'],
  )
endif

clang_tidy = find_program('clang-tidy', required : false)
if clang_tidy.found()
  cpp_only_output = run_command(
    py,
    list_sources,
    'src',
    '--glob', '*.cpp',
    check : true,
  ).stdout().strip()
  cpp_only_files = []
  if cpp_only_output != ''
    foreach cpp_file : cpp_only_output.split('\n')
      cpp_only_files += [meson.project_source_root() / cpp_file]
    endforeach
  endif
  if cpp_only_files.length() > 0
    run_target(
      'analyze',
      command : [clang_tidy, '-p', meson.project_build_root()] + cpp_only_files,
    )
  endif
endif
