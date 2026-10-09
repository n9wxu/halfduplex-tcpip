# Host tests: the slave (link485, linkcheck) on the test HAL with the real IP
# stack above it, driven by a TMP-v2 master in the test itself.

hdx_warnings(master.c test_link.c test_linkcheck.c template_links.c
             ${PROJECT_SOURCE_DIR}/port/template/hdx_hal_template.c)

add_library(master STATIC master.c)
target_include_directories(master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(master PUBLIC hdx_slave_test)

add_executable(test_link test_link.c)
target_link_libraries(test_link PRIVATE master)
add_test(NAME link COMMAND test_link)

add_executable(test_linkcheck test_linkcheck.c)
target_link_libraries(test_linkcheck PRIVATE master)
add_test(NAME linkcheck COMMAND test_linkcheck)

# The template port must stay complete: a slave links against it alone.
add_executable(test_template_links template_links.c ${PROJECT_SOURCE_DIR}/port/template/hdx_hal_template.c)
target_link_libraries(test_template_links PRIVATE hdx_slave hdx_stack_host)
add_test(NAME template_links COMMAND test_template_links)
