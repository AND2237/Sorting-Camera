cmake_minimum_required(VERSION 3.21)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network Test)

function(scam_add_test name)
    qt_add_executable(${name} ${name}.cpp)
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
    target_link_libraries(${name} PRIVATE scamcore Qt6::Test)
    # The raw artifacts under benchmarks/results/ are the ground truth every
    # published benchmark figure has to resolve against. Provenance tests read
    # them at run time rather than trusting a table transcribed by hand, which
    # is how a figure from one configuration came to be published on another.
    target_compile_definitions(${name} PRIVATE
        SCAM_BENCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/results")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

scam_add_test(tst_authclient)
scam_add_test(tst_credentialstore)
scam_add_test(tst_discovery)
scam_add_test(tst_sessionstate)
scam_add_test(tst_userprefs)
scam_add_test(tst_devicestatus)
scam_add_test(tst_deviceregistry)
scam_add_test(tst_profileengine)
scam_add_test(tst_diagnostics)
scam_add_test(tst_notificationcenter)
scam_add_test(tst_recorder)
scam_add_test(tst_capture)
scam_add_test(tst_configwritesequence)
scam_add_test(tst_capabilities)
# Section 33 / audit section 14: the metric arithmetic behind every published
# frame-rate figure had no coverage until this suite.
scam_add_test(tst_appmetrics)
scam_add_test(tst_accessibility)
# Source-level accessibility tripwire (section 25 / gap G-12): it reads the QML
# itself, the way the provenance tests read benchmarks/results.
target_compile_definitions(tst_accessibility PRIVATE
    SCAM_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../desktop/qml")
