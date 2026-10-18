include(FetchContent)

# Sources only: each is compiled here with this project's own settings, so the
# packages' build files are not used (SOURCE_SUBDIR names a directory that does
# not exist).

# The last release: 0.1.12 measures the round trip (RFC 6298), so the
# retransmission timeout comes back down after a loss
# (https://github.com/n9wxu/smallest_tcp/issues/12).
FetchContent_Declare(smallest_tcp
    GIT_REPOSITORY https://github.com/n9wxu/smallest_tcp.git
    GIT_TAG        v0.1.12
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _sources_only)
FetchContent_MakeAvailable(smallest_tcp)

if(HDX_PORT STREQUAL "samd21")
    FetchContent_Declare(cmsis_atmel
        GIT_REPOSITORY https://github.com/arduino/ArduinoModule-CMSIS-Atmel.git
        GIT_TAG        v1.2.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _sources_only)
    FetchContent_Declare(cmsis_core
        GIT_REPOSITORY https://github.com/ARM-software/CMSIS_6.git
        GIT_TAG        v6.1.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _sources_only)
    FetchContent_MakeAvailable(cmsis_atmel cmsis_core)
endif()

# The IP stack (smallest_tcp) as the slave uses it: IPv4, ARP, ICMP, UDP, TFTP
# and, with `with_tcp`, TCP with stop-and-wait buffers.
function(hdx_stack name with_tcp)
    set(src ${smallest_tcp_SOURCE_DIR}/src)
    add_library(${name} STATIC
        ${src}/net.c ${src}/net_cksum.c ${src}/net_text.c ${src}/eth.c ${src}/arp.c ${src}/ipv4.c
        ${src}/icmp.c ${src}/udp.c ${src}/tftp.c)
    if(with_tcp)
        target_sources(${name} PRIVATE ${src}/tcp.c ${src}/tcp_buf_saw.c)
    endif()
    target_include_directories(${name} PUBLIC ${smallest_tcp_SOURCE_DIR}/include)
    target_compile_definitions(${name} PUBLIC
        NET_USE_IPV4=1 NET_USE_IPV6=0 NET_USE_UDP=1 NET_USE_TCP=$<BOOL:${with_tcp}>
        NET_MAX_MCAST_GROUPS=0)
endfunction()
