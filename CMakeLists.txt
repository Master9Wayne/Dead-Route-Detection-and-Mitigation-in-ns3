build_exec(
  EXECNAME drdm
  SOURCE_FILES
    drdm.cc
    drdm-probe-app.cc
    drdm-traffic-app.cc
  LIBRARIES_TO_LINK
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libpoint-to-point}
    ${libapplications}
    ${libflow-monitor}
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/drdm
)
