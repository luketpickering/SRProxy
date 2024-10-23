#pragma once

#include "Rtypes.h"

#include <array>
#include <cassert>
#include <cmath> // for std::isinf and std::isnan
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

class TFormLeafInfo;
class TBranch;
class TLeaf;
class TTreeFormula;
class TTree;

namespace caf {
/// this constant is passed by reference into the various Proxy constructors.
inline const long kDummyBase = 0;

class SRBranchRegistry {
public:
  static void AddBranch(const std::string &b) { fgBranches.insert(b); }
  static const std::set<std::string> &GetBranches() { return fgBranches; }
  static void clear() { fgBranches.clear(); }

  static void Print(bool abbrev = true);
  static void ToFile(const std::string &fname);

protected:
  static std::set<std::string> fgBranches;
};

enum CAFType {
  kNested,
  kFlat,
  kCopiedRecord // Assigned into, not associated with a file
};

CAFType GetCAFType(TTree *tr);

/// Count the subscripts in the name
int NSubscripts(const std::string &name);

template <class T> struct is_vec {
  static const bool value = false;
};
template <class T> struct is_vec<std::vector<T>> {
  static const bool value = true;
};

template <class T> class Proxy;

class Restorer;

/// Base class for all proxy types, intended to help trace ancestry
class Lineage {
public:
  /// note: this is NOT a copy constructor!  Specifies the object that's this
  /// one's parent.
  explicit Lineage(const Lineage *parent) : fParent(parent) {}
  virtual ~Lineage() = default;

  /// \brief Search through the stored lineage to find ancestor of given type.
  /// If multiple, returns only the closest one.
  /// \tparam T   Object type to look for (e.g.: SRProxy)
  /// \return  Pointer to closest ancestor (fewest links separating them) of
  /// type T, or nullptr if none found
  template <typename T> const T *Ancestor() const {
    const Lineage *candAnc = this;
    while ((candAnc = candAnc->Parent())) {
      if (auto ret = dynamic_cast<const T *>(candAnc)) {
        return ret;
      }
    }
    return nullptr;
  }

  const Lineage *Parent() const { return fParent; }

private:
  const Lineage *fParent = nullptr;
};

template <class T> class Proxy : public Lineage {
public:
  static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                    std::is_same_v<T, std::string>,
                "Invalid type for basic type Proxy");

  friend class Restorer;

  Proxy(TTree *tr, const std::string &name, const long &base, int offset,
        const Lineage *parent = nullptr);
  Proxy(TTree *tr, const std::string &name)
      : Proxy(tr, name, kDummyBase, 0, nullptr) {}

  // Need to be copyable because Vars return us directly
  Proxy(const Proxy &);
  Proxy(const Proxy &&);
  // No need to be assignable though
  Proxy &operator=(const Proxy &) = delete;

  // Somehow including this helps us not get automatically converted to a
  // type we might not want to be in ternary expressions (we now get a type
  // error instead).
  Proxy(T v) = delete;

  ~Proxy();

  operator T() const { return GetValueChecked(); }

  T GetValue() const;

  // In practice these are the only operations that systematic shifts use
  Proxy<T> &operator=(T x);
  Proxy<T> &operator+=(T x);
  Proxy<T> &operator-=(T x);
  Proxy<T> &operator*=(T x);

  std::string Name() const { return fName; }

  void CheckEquals(const T &x) const;

protected:
  // Print a warning on inf or NaN
  T GetValueChecked() const;

  T GetValueNested() const;
  T GetValueFlat() const;

  void SetShifted();

  // The type to fetch from the TLeaf - get template errors inside of ROOT
  // for enums.
  typedef typename std::conditional_t<std::is_enum_v<T>, int, T> U;

  // Shared
  std::string fName;
  CAFType fType;
  mutable TLeaf *fLeaf;
  mutable U fVal;
  TTree *fTree;

  // Flat
  const long &fBase;
  int fOffset;

  // Nested
  mutable TFormLeafInfo *fLeafInfo;
  mutable TBranch *fBranch;
  mutable std::unique_ptr<TTreeFormula> fTTF;
  mutable long fEntry;
  mutable int fSubIdx;
};

// Helper functions that don't need to be templated
class ArrayVectorProxyBase : public Lineage {
public:
  std::string Name() const { return fName; }

protected:
  ArrayVectorProxyBase(TTree *tr, const std::string &name,
                       bool isNestedContainer, const long &base, int offset,
                       const Lineage *parent = nullptr);

  ~ArrayVectorProxyBase() = default;

  void EnsureIdxP() const;

  void CheckIndex(size_t i, size_t size) const;

  std::string IndexField() const;

  /// add [i], or something more complex for nested CAFs
  std::string Subscript(int i) const;

  std::string SubName() const;

  // Trivial, but requires including TTree.h, which we don't want in header
  bool TreeHasLeaf(TTree *tr, const std::string &name) const;

  TTree *fTree;
  std::string fName;
  bool fIsNestedContainer;
  CAFType fType;
  const long &fBase;
  int fOffset;
  mutable std::unique_ptr<Proxy<long long>> fIdxP;
  mutable long fIdx;
};

// Helper functions that don't need to be templated
class VectorProxyBase : public ArrayVectorProxyBase {
public:
  ~VectorProxyBase() = default;

  size_t size() const;
  bool empty() const;
  void resize(size_t i);

protected:
  VectorProxyBase(TTree *tr, const std::string &name, bool isNestedContainer,
                  const long &base, int offset,
                  const Lineage *parent = nullptr);

  std::string LengthField() const;
  /// Helper for LengthField()
  std::string NName() const;

  void EnsureSizeExists() const;
  mutable std::unique_ptr<Proxy<int>> fSize; ///< only initialized on-demand
};

template <class T> class Proxy<std::vector<T>> : public VectorProxyBase {
public:
  Proxy(TTree *tr, const std::string &name, const long &base, int offset,
        const Lineage *parent)
      : VectorProxyBase(tr, name, is_vec<T>::value || std::is_array_v<T>, base,
                        offset, parent) {}

  Proxy(TTree *tr, const std::string &name)
      : Proxy(tr, name, kDummyBase, 0, nullptr) {}

  ~Proxy() = default;

  Proxy &operator=(const Proxy<std::vector<T>> &) = delete;
  Proxy(const Proxy<std::vector<T>> &v) = delete;

  Proxy<T> &at(size_t i) const {
    EnsureLongEnough(i);
    return *fElems[i];
  }
  Proxy<T> &at(size_t i) {
    EnsureLongEnough(i);
    return *fElems[i];
  }

  Proxy<T> &operator[](size_t i) const { return at(i); }
  Proxy<T> &operator[](size_t i) { return at(i); }

  template <class U> Proxy<std::vector<T>> &operator=(const std::vector<U> &x) {
    resize(x.size());
    for (unsigned int i = 0; i < x.size(); ++i)
      at(i) = x[i];
    return *this;
  }

  template <class U> void CheckEquals(const std::vector<U> &x) const {
    EnsureSizeExists();
    fSize->CheckEquals(x.size());
    for (unsigned int i = 0; i < std::min(size(), x.size()); ++i)
      at(i).CheckEquals(x[i]);
  }

  // U should be either T or const T
  template <class U> class iterator {
  public:
    Proxy<T> &operator*() { return (*fParent)[fIdx]; }
    iterator<U> &operator++() {
      ++fIdx;
      return *this;
    }
    bool operator!=(const iterator<U> &it) const { return fIdx != it.fIdx; }
    bool operator==(const iterator<U> &it) const { return fIdx == it.fIdx; }

  protected:
    friend class Proxy<std::vector<T>>;
    iterator(const Proxy<std::vector<T>> *p, int i) : fParent(p), fIdx(i) {}

    const Proxy<std::vector<T>> *fParent;
    size_t fIdx;
  };

  iterator<const T> begin() const { return iterator<const T>(this, 0); }
  iterator<T> begin() { return iterator<T>(this, 0); }
  iterator<const T> end() const { return iterator<const T>(this, size()); }
  iterator<T> end() { return iterator<T>(this, size()); }

protected:
  /// Implies CheckIndex()
  void EnsureLongEnough(size_t i) const {
    CheckIndex(i, size());
    if (i >= fElems.size()) {
      fElems.resize(i + 1);
    }

    EnsureIdxP();
    if (fIdxP) {
      fIdx = *fIdxP; // store into an actual value we can point to
    }
    // note that the contained elements should point to the vector's parent, not
    // the vector
    if (!fElems[i]) {
      fElems[i] = std::make_unique<Proxy<T>>(fTree, Subscript(i), fIdx, i,
                                             this->Parent());
    }
  }

  mutable std::vector<std::unique_ptr<Proxy<T>>> fElems;
};

// Retain an alias to the old naming scheme for now
template <class T> using VectorProxy = Proxy<std::vector<T>>;

/// Used in comparison of GENIE version numbers
template <class T>
bool operator<(const Proxy<std::vector<T>> &a, const std::vector<T> &b) {
  const size_t N = a.size();
  if (N != b.size()) {
    return N < b.size();
  }
  for (size_t i = 0; i < N; ++i) {
    if (a[i] != b[i]) {
      return a[i] < b[i];
    }
  }
  return false;
}

template <class T, unsigned int N>
class Proxy<T[N]> : public ArrayVectorProxyBase {
public:
  Proxy(TTree *tr, const std::string &name, const long &base, int offset,
        const Lineage *parent)
      : ArrayVectorProxyBase(tr, name, is_vec<T>::value || std::is_array_v<T>,
                             base, offset) {
    for (auto &el : fElems) {
      el = nullptr;
    }
  }

  Proxy(TTree *tr, const std::string &name)
      : Proxy(tr, name, kDummyBase, 0, nullptr) {}

  ~Proxy() = default;

  Proxy &operator=(const Proxy<T[N]> &) = delete;
  Proxy(const Proxy<T[N]> &v) = delete;

  const Proxy<T> &operator[](size_t i) const {
    EnsureElem(i);
    if (fIdxP) {
      fIdx = *fIdxP;
    }
    return *fElems[i];
  }
  Proxy<T> &operator[](size_t i) {
    EnsureElem(i);
    if (fIdxP) {
      fIdx = *fIdxP;
    }
    return *fElems[i];
  }

  Proxy<T[N]> &operator=(const T (&x)[N]) {
    for (unsigned int i = 0; i < N; ++i) {
      (*this)[i] = x[i];
    }
    return *this;
  }

  void CheckEquals(const T (&x)[N]) const {
    for (unsigned int i = 0; i < N; ++i) {
      (*this)[i].CheckEquals(x[i]);
    }
  }

protected:
  void EnsureElem(int i) const {
    CheckIndex(i, N);
    if (fElems[i]) {
      return; // element already created
    }
    if (fType != kFlat || TreeHasLeaf(fTree, IndexField())) {
      // Regular out-of-line array, handled the same as a vector.
      EnsureIdxP();
      fElems[i] =
          std::make_unique<Proxy<T>>(fTree, Subscript(i), fIdx, i, nullptr);
    } else {
      // No ..idx field implies this is an "inline" array where the elements
      // are in individual branches like foo.0.bar
      const std::string dotname = fName + "." + std::to_string(i);
      fElems[i] =
          std::make_unique<Proxy<T>>(fTree, dotname, fBase, fOffset, nullptr);
    }
  }

  mutable std::array<std::unique_ptr<Proxy<T>>, N> fElems;
};

// Retain an alias to the old naming scheme for now
template <class T, unsigned int N> using ArrayProxy = Proxy<T[N]>;

template <class T> class RestorerT {
public:
  ~RestorerT() {
    // Restore values in reverse, i.e. in first-in, last-out order so that if
    // a value was edited multiple time it will eventually be restored to its
    // original value.
    for (auto &[p, v] : fVals) {
      p = v;
    }
  }

  void Add(T &p, T v) { fVals.emplace_back(p, v); }

protected:
  std::vector<std::pair<std::reference_wrapper<T>, T>> fVals;
};

class Restorer : public RestorerT<char>,
                 RestorerT<short>,
                 RestorerT<int>,
                 RestorerT<long>,
                 RestorerT<long long>,
                 RestorerT<unsigned char>,
                 RestorerT<unsigned short>,
                 RestorerT<unsigned int>,
                 RestorerT<unsigned long>,
                 RestorerT<unsigned long long>,
                 RestorerT<float>,
                 RestorerT<double>,
                 RestorerT<long double>,
                 RestorerT<bool>,
                 RestorerT<std::string> {
public:
  template <class T> void Add(Proxy<T> &p) {
    RestorerT<typename Proxy<T>::U>::Add(p.fVal, p.GetValue());
  }
};

class SRProxySystController {
public:
  static bool AnyShifted() {
    for (auto const &r : fRestorers) {
      if (r) {
        return true;
      }
    }
    return false;
  }

  static void BeginTransaction() { fRestorers.push_back(nullptr); }

  static bool InTransaction() { return !fRestorers.empty(); }

  static void Rollback() {
    if (fRestorers.empty()) {
      throw std::runtime_error(
          "During SRProxySystController::Rollback, fRestorers was empty.");
    }

    if (fRestorers.back()) {
      ++fGeneration;
    }
    fRestorers.pop_back();
  }

  /// May be useful in the implementation of caches that ought to be
  /// invalidated when systematic shifts are applied.
  static long long Generation() {
    if (!InTransaction()) {
      return 0; // nominal
    }
    return fGeneration;
  }

protected:
  template <class T> friend class Proxy;

  template <class T> static void Backup(Proxy<T> &p) {
    assert(!fRestorers.empty());
    if (!fRestorers.back()) {
      ++fGeneration;
      fRestorers.back() = std::make_unique<Restorer>();
    }
    fRestorers.back()->Add(p);
  }

  static std::vector<std::unique_ptr<Restorer>> fRestorers;
  static long long fGeneration;
};

} // namespace caf

namespace std {
template <class T> T min(const caf::Proxy<T> &a, T b) {
  return std::min(a.GetValue(), b);
}

template <class T> T min(T a, const caf::Proxy<T> &b) {
  return std::min(a, b.GetValue());
}

template <class T> T max(const caf::Proxy<T> &a, T b) {
  return std::max(a.GetValue(), b);
}

template <class T> T max(T a, const caf::Proxy<T> &b) {
  return std::max(a, b.GetValue());
}

// We override these two so that the callers don't trigger the warning
// printout from operator T.
template <class T> bool isnan(const caf::Proxy<T> &x) {
  return std::isnan(x.GetValue());
}

template <class T> bool isinf(const caf::Proxy<T> &x) {
  return std::isinf(x.GetValue());
}
} // namespace std

// There are also versions of these not in std:: that we want to override
using std::isinf;
using std::isnan;
