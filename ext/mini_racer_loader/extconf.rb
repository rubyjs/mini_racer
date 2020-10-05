require 'mkmf'

extension_name = 'mini_racer_loader'
dir_config extension_name

$CPPFLAGS += " -fvisibility=hidden "

create_makefile extension_name
