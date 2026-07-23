#include "lsm/engine.h"

using namespace ::tiny_lsm;

int main() {
  LSM lsm("test");
  // change the data manually
  lsm.put("k1", "v1");
  lsm.clear();
}
