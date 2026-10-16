#ifndef COOP_TEST_H
#define COOP_TEST_H

// Test-only actions (see CoopTest.cpp). Game thread.
namespace Coop::Test {

// Called once per frame from the coop frame hook.
void Frame();

} // namespace Coop::Test

#endif // COOP_TEST_H
