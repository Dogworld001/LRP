idf_component_register(
    SRCS
        "main.cpp"
        "../../../demo/demo_app.cpp"
        "../../../demo/bench_app.cpp"
        "../../../demo/peer_app.cpp"
        "../../../src/core.cpp"
        "../../../src/xport.cpp"
        "../../../src/aead.cpp"
    INCLUDE_DIRS
        "../../../include"
        "../../../src"
        "../../../demo"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        lwip
)

# Xtensa 的 uint32_t 为 unsigned long(LP32),与 %u 的 -Wformat 检查冲突;
# 运行时宽度一致、行为正确,故仅对该类警告降级(-Wno-format)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
