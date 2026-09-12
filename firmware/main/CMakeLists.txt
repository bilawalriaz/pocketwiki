execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_style.py"
    RESULT_VARIABLE POCKETWIKI_STYLE_RESULT
)
if(NOT POCKETWIKI_STYLE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate embedded PocketWiki stylesheet")
endif()

execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_catalog.py"
    RESULT_VARIABLE POCKETWIKI_CATALOG_RESULT
)
if(NOT POCKETWIKI_CATALOG_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate embedded PocketWiki pack catalogue")
endif()

execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_manager_js.py"
    RESULT_VARIABLE POCKETWIKI_MANAGER_JS_RESULT
)
if(NOT POCKETWIKI_MANAGER_JS_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate embedded PocketWiki manager JavaScript")
endif()

set(PWPK_ASSET_SRCS "")
if(CONFIG_POCKETWIKI_PWPK_SERVICE)
    set(PWPK_ASSET_SRCS "${CMAKE_CURRENT_BINARY_DIR}/pwpk_assets.c")
    execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_pwpk_browser.py"
        --output "${PWPK_ASSET_SRCS}" RESULT_VARIABLE PWPK_ASSET_RESULT)
    if(NOT PWPK_ASSET_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not generate offline PWPK browser assets")
    endif()
endif()

set(PWPK_BENCH_SRCS "")
if(CONFIG_POCKETWIKI_SERIAL_BENCHMARK)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/pwpk_serial_fixture.c")
        message(FATAL_ERROR "Generate benchmark fixtures with tools/benchmark_device.py --serial-benchmark")
    endif()
    list(APPEND PWPK_BENCH_SRCS "pwpk_serial_benchmark.c" "pwpk_serial_fixture.c")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "wifi_ap.c"
        "web_server.c"
        "content_archive.c"
        "title_lookup.c"
        "html_helpers.c"
        "oled_display.c"
        "qr_code.c"
        "boot_button.c"
        "pack_store.c"
        "pwpk_service.c"
        "pwpk_http.c"
        "pwpk_reader.c"
        ${PWPK_BENCH_SRCS}
        ${PWPK_ASSET_SRCS}
        "ble_provisioning.c"
        "style_asset.c"
        "catalog_asset.c"
        "manager_js_asset.c"
        "../third_party/miniz/miniz.c"
    INCLUDE_DIRS "." "../third_party/miniz"
    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash esp_lcd esp_driver_i2c
             esp_partition esp_rom esp_timer app_update spi_flash bt spiffs fatfs wear_levelling
             cjson esp_http_client mbedtls
    PRIV_REQUIRES esp_driver_gpio
)

# Vendored miniz trips a few -Werror type-range checks on 32-bit targets;
# relax only those for this one file (it is third-party code, not ours).
set_source_files_properties("../third_party/miniz/miniz.c" PROPERTIES
    COMPILE_FLAGS "-Wno-error=type-limits")
