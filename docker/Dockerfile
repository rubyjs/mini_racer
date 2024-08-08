FROM ubuntu:noble AS build

ARG v8_version=12.4.254.21
ENV version $v8_version

ENV target x86.static
ENV GYP_GENERATORS ninja
ENV PATH "/depot_tools:${PATH}"
ENV pkgdir /package

RUN apt-get update -y && \
	apt-get upgrade -y && \
	apt-get install -y curl python3 git pkg-config libglib2.0-dev clang xz-utils libc++-dev libc++abi-dev llvm lld

RUN git clone https://chromium.googlesource.com/chromium/tools/depot_tools

RUN yes | fetch v8

WORKDIR /v8

RUN gclient sync -D --force --reset && \
	gclient sync -D --revision ${version}

RUN gn gen ${target} -vv --fail-on-unused-args \
    --args='v8_monolithic=true \
            v8_static_library=true \
            is_component_build=false \
            v8_enable_sandbox=false \
            clang_use_chrome_plugins=false \
            is_asan=false \
            is_debug=false \
            treat_warnings_as_errors=false \
            v8_enable_i18n_support=false\
            v8_use_external_startup_data=false \
            use_custom_libcxx=false \
            v8_enable_pointer_compression=false \
            use_sysroot=false'

RUN ninja -C ${target} "v8_monolith"

RUN	install -d ${pkgdir}v8 && \
	install -d ${pkgdir}/v8/lib && \
	install -Dm755 ${target}/obj/libv8_monolith.a ${pkgdir}/v8/lib/libv8_monolith.a && \
	install -d ${pkgdir}/v8/include && \
	install -Dm644 include/*.h ${pkgdir}/v8/include && \
	install -d ${pkgdir}/v8/include/cppgc && \
	install -Dm644 include/cppgc/*.h ${pkgdir}/v8/include/cppgc && \
	install -d ${pkgdir}/v8/include/libplatform && \
	install -Dm644 include/libplatform/*.h ${pkgdir}/v8/include/libplatform && \
	install -d ${pkgdir}/v8/lic/ && \
	install -m644 LICENSE* ${pkgdir}/v8/lic/
