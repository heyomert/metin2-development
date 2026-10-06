#include "m2_build_identity.h"

#define M2_COMPONENT "db"
#define M2_VERSION_FILE "VERSION.txt"
#define M2_VERSION_EXIT_ON_FAIL 1 // upstream behaviour: db stops when it cannot write its version file
#include "common/build_identity_impl.h"
