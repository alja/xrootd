
FIND_PATH(XROOTD_INCLUDES XrdVersion.hh
  HINTS
  ${XROOTD_DIR}
  $ENV{XROOTD_DIR}
  /usr
  /opt/xrootd/
  PATH_SUFFIXES include/xrootd
  PATHS /opt/xrootd
)

FIND_PATH(XROOTD_PRIVATE_INCLUDES XrdHttp/XrdHttpExtHandler.hh
  HINTS
  ${XROOTD_DIR}/private
  $ENV{XROOTD_DIR}/private
  /usr
  /opt/xrootd/
  PATH_SUFFIXES include/xrootd/private
  PATHS /opt/xrootd
)

FIND_LIBRARY(XROOTD_UTILS_LIB XrdUtils
  HINTS
  ${XROOTD_DIR}
  $ENV{XROOTD_DIR}
  /usr
  /opt/xrootd/
  PATH_SUFFIXES lib
)

FIND_LIBRARY(XROOTD_SERVER_LIB XrdServer
  HINTS
  ${XROOTD_DIR}
  $ENV{XROOTD_DIR}
  /usr
  /opt/xrootd/
  PATH_SUFFIXES lib
)

FIND_LIBRARY(XROOTD_HTTP_LIB XrdHttp-4
  HINTS
  ${XROOTD_DIR}
  $ENV{XROOTD_DIR}
  /usr
  /opt/xrootd/
  PATH_SUFFIXES lib
)

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Xrootd DEFAULT_MSG XROOTD_UTILS_LIB XROOTD_HTTP_LIB XROOTD_INCLUDES XROOTD_PRIVATE_INCLUDES)

