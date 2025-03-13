// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <fmt/format.h>

#include "common/ceph_argparse.h"
#include "common/ceph_context.h"
#include "global/global_context.h"
#include "global/global_init.h"

#include "include/ceph_assert.h"

#include "gtest/gtest.h"

static void trigger_assert() {
  ceph_assert(42 == 41);
}

TEST(CephAssertDeathTest, NotRecursive) {
  ASSERT_DEATH(trigger_assert(), "FAILED ceph_assert(recursive || !is_locked_by_me())");
  ceph_assert(42 == 24);
}

int main(int argc, char **argv) {
  auto args = argv_to_vec(argc, argv);

  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  g_ceph_context->_conf.set_val("disaster_recovery_bypass_assert", fmt::format("{}:{}", __FILE__, 21));
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
