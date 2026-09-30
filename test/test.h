#include <string>
#include <vector>

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

enum eA {
 kA = 0,
 kB,
 kC
};

enum class eB : std::size_t {
  ka = 500,
  kb = 1000,
  kc = 1500
};

enum class eC {
  kaa = -500,
  kbb = -1000,
  kcc = -1500
};

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

  // void setA(eA const &){}
  // void setB(eB const &){}
  // void setC(std::vector<eC> const &){}
};

#endif

} // namespace test
