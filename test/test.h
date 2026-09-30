#include <string>
#include <vector>

#include "testEnums.h"

namespace test {

struct TestBase {
  int bA;
  float bB;
};

struct TestConstituentC {
  NEEDTHISTOBUILD A;
  float B;
  double C;
};

struct TestConstituentA {
  int A;
  float B;
  double C;
  bool D;
  std::string E;
};

struct TestConstituentB {
  std::vector<float> B;
};

struct TestConstituentD {
  int a,b;
};

#ifdef REVEAL_TestTarget

struct TestTarget : public TestBase {
  int A;
  float B;
  double C[10][4];
  bool D;
  std::string E;

  eA myen;

  static const int F = 5;

  TestConstituentC aC[5];

  std::vector<TestConstituentA> vA;
  std::vector<std::vector<TestConstituentB>> vvB;
  std::vector<std::vector<std::vector<std::string>>> vvvs;

  void UseD(TestConstituentD const &){}
  void SetBs(std::vector<eB> const &){}
};

#endif

} // namespace test
