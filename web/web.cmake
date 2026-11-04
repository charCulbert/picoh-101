# The GUI page's TypeScript (web/src), typechecked and bundled into web/dist (app.js, and bridge.js which
# only the plugin loads), for the builds that carry the page. Needs Node: the first build installs the
# pinned TypeScript and esbuild (web/package-lock.json) into web/node_modules.
# Gives web_dist, a target to depend on, and WEB_DIST_FILES, the files it makes.
set(WEB_DIR ${CMAKE_CURRENT_LIST_DIR})
find_program(NPM NAMES npm.cmd npm REQUIRED)
file(GLOB WEB_TS CONFIGURE_DEPENDS ${WEB_DIR}/src/*.ts ${WEB_DIR}/lib/*.d.ts)
file(GLOB_RECURSE WEB_COMPOST CONFIGURE_DEPENDS ${WEB_DIR}/vendor/compost/src/*.js)
set(WEB_DIST_FILES ${WEB_DIR}/dist/app.js ${WEB_DIR}/dist/bridge.js)

add_custom_command(
    OUTPUT ${WEB_DIR}/node_modules/.package-lock.json
    COMMAND ${NPM} ci --no-audit --no-fund
    WORKING_DIRECTORY ${WEB_DIR}
    DEPENDS ${WEB_DIR}/package-lock.json)
add_custom_command(
    OUTPUT ${WEB_DIST_FILES}
    COMMAND ${NPM} run build --silent
    WORKING_DIRECTORY ${WEB_DIR}
    DEPENDS ${WEB_TS} ${WEB_COMPOST} ${WEB_DIR}/package.json ${WEB_DIR}/tsconfig.json
            ${WEB_DIR}/node_modules/.package-lock.json)
if(NOT TARGET web_dist)
    add_custom_target(web_dist DEPENDS ${WEB_DIST_FILES})
endif()
