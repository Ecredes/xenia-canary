#include <cstring>
#include <vector>
#include <string>

namespace Bad_Namespace {

// TODO: unattributed todo for cpplint and clang-tidy.
class lower_class {
 public:
  int Value() { return value; }

 private:
  int value;
};

int NullDeref() {
  int* p = nullptr;
  return *p;
}

void Copy(char* dst, const char* src) { strcpy(dst, src); }

void Swap(int a, int b) {
  if (a = b) {
    return;
  }
}

}  // namespace Bad_Namespace
