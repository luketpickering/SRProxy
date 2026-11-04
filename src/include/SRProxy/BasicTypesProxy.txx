#pragma once

#include "SRProxy/BasicTypesProxy.h"

#include "SRProxy/Exceptions.h"

#include "TError.h"
#include "TFile.h"
#include "TFormLeafInfo.h"
#include "TTreeFormula.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace std::string_literals;

namespace {
/// Helper for CheckEquals
template <class T> bool AreEqual(const T &x, const T &y) {
  if constexpr (std::is_floating_point_v<T>) {
    return x == y || (std::isnan(x) && std::isnan(y));
  } else {
    return x == y;
  }
}

/// Helper class to track inf/nans encountered.
class InfNanTable {
private:
  struct Encounters {
    std::size_t count = 0; /// total number of times this var saw a NaN/inf
    std::string firstFile; /// first file a NaN/inf was seen in
    std::size_t firstEntry =
        std::numeric_limits<std::size_t>::max(); /// entry number within
                                                 /// firstFile where the first
                                                 /// NaN/inf was seen
  };

public:
  ~InfNanTable() noexcept { EmitTable(); }

  void LogInf(const std::string &varPath, const char *file, std::size_t entry) {
    Log(fInfEncounters, varPath, file, entry);
    CheckAbort();
  };
  void LogNaN(const std::string &varPath, const char *file, std::size_t entry) {
    Log(fNaNEncounters, varPath, file, entry);
    CheckAbort();
  };

  void EmitTable(std::ostream &stream = std::cerr) const noexcept {
    bool showedHeader = false;
    bool anyWarns = false;
    for (const auto encounters : {&fNaNEncounters, &fInfEncounters}) {
      if (encounters->empty()) {
        continue;
      }

      anyWarns = true;
      if (!showedHeader) {
        stream << "\n\n\x1B[33mWARNING:\033[0m\n";
        showedHeader = true;
      }
      stream << "\n\x1B[33mSRProxy encountered "
             << (encounters == &fNaNEncounters ? "NaN" : "inf")
             << " in the following variables:\033[0m\n";
      for (const auto &encounterPair : *encounters) {
        stream << "  '\x1B[95m" << encounterPair.first << "\033[0m' ("
               << encounterPair.second.count << " encounters)\n";
        if (!encounterPair.second.firstFile.empty())
          stream << "     first encountered in entry "
                 << encounterPair.second.firstEntry
                 << " of file: " << encounterPair.second.firstFile << "\n";
      } // for (encounterPair)
    } // for (encounters)

    if (anyWarns) {
      stream << "\nSet environment variable SRPROXY_ABORT_ON_INFNAN=1 to "
                "instead abort immediately when inf/NaN is encountered.\n";
    }
  } // InfNaNTable::EmitTable()

private:
  /// Set environment variable SRPROXY_ABORT_ON_INFNAN=1 to abort immediately
  /// when an inf or NaN is encountered
  void CheckAbort() const {
    // user can tell us to abort immediately if any NaNs/infs are found.
    // (useful for debugging)
    static bool checkedVar = false;
    if (!checkedVar) {
      if (auto val = std::getenv("SRPROXY_ABORT_ON_INFNAN")) {
        if (std::string(val) != "0") {
          EmitTable(std::cerr);
          std::cerr << "Aborting on first inf/NaN per configuration.  Unset "
                       "$SRPROXY_ABORT_ON_INFNAN to disable this behavior.\n";
          abort();
        }
      } // if ( val )
      checkedVar = true;
    } // if (!checkedVar)
  } // CheckAbort()

  static void Log(std::map<std::string, Encounters> &encountersMap,
                  const std::string &varPath, const char *file,
                  std::size_t entry) {
    Encounters &encounters =
        encountersMap[varPath]; // will default-construct if not found
    if (encounters.count++ == 0) {
      encounters.firstFile = file;
      encounters.firstEntry = entry;
    }
  }

  std::map<std::string, Encounters> fNaNEncounters;
  std::map<std::string, Encounters> fInfEncounters;
};

InfNanTable infNanTable;
} // namespace

namespace caf {

//----------------------------------------------------------------------
inline std::string StripSubscripts(const std::string &s) {
  std::string ret;
  ret.reserve(s.size());
  bool insub = false;
  for (char c : s) {
    /**/ if (c == '[') {
      insub = true;
    } else if (c == ']') {
      insub = false;
    } else if (!insub) {
      ret += c;
    }
  }
  return ret;
}

template <class T>
Proxy<T>::Proxy(TTree *tr, const std::string &name, const long &base,
                int offset, const Lineage *parent)
    : Lineage(parent), fName(name), fType(GetCAFType(tr)), fLeaf(0), fTree(tr),
      fBase(base), fOffset(offset), fLeafInfo(0), fBranch(0), fTTF(nullptr),
      fEntry(-1), fSubIdx(0) {}

const long kDummyBaseUninit = -1;

//----------------------------------------------------------------------
template <class T>
Proxy<T>::Proxy(const Proxy<T> &p)
    : Lineage(&p), fName("copy of " + p.fName), fType(kCopiedRecord), fLeaf(0),
      fTree(0), fBase(kDummyBaseUninit), fOffset(-1), fLeafInfo(0), fBranch(0),
      fTTF(nullptr), fEntry(-1), fSubIdx(-1) {
  // Ensure that the value is evaluated and baked in in the parent object, so
  // that fTTF et al aren't re-evaluated in every single copy.
  fVal = p.GetValue();
}

//----------------------------------------------------------------------
template <class T>
Proxy<T>::Proxy(const Proxy &&p)
    : Lineage(std::move(p)), fName("move of " + p.fName), fType(kCopiedRecord),
      fLeaf(0), fTree(0), fBase(kDummyBaseUninit), fOffset(-1), fLeafInfo(0),
      fBranch(0), fTTF(nullptr), fEntry(-1), fSubIdx(-1) {
  // Ensure that the value is evaluated and baked in in the parent object, so
  // that fTTF et al aren't re-evaluated in every single copy.
  fVal = p.GetValue();
}

//----------------------------------------------------------------------
template <class T> Proxy<T>::~Proxy() {}

//----------------------------------------------------------------------
template <class T> T Proxy<T>::GetValue() const {
  switch (fType) {
  case kNested: {
    return GetValueNested();
  }
  case kFlat: {
    return GetValueFlat();
  }
  case kCopiedRecord: {
    return static_cast<T>(fVal);
  }
  default: {
    std::stringstream ss;
    ss << "Proxy<T>::GetValue() called for invalid fType: " << fType;
    throw std::runtime_error(ss.str());
  }
  }
}

//----------------------------------------------------------------------
template <class T> T Proxy<T>::GetValueChecked() const {
  const T val = GetValue();

  if constexpr (std::is_floating_point_v<T>) {
    const char *filename =
        (fTree && fTree->GetDirectory() && fTree->GetDirectory()->GetFile())
            ? fTree->GetDirectory()->GetFile()->GetName()
            : "";
    if (isnan(val)) {
      ::infNanTable.LogNaN(fName, filename, fEntry);
    } else if (isinf(val)) {
      ::infNanTable.LogInf(fName, filename, fEntry);
    }
  }

  return val;
}

//----------------------------------------------------------------------
template <class T> void GetTypedValueWrapper(TLeaf *leaf, T &x, int subidx) {
  if constexpr (std::is_same_v<T, std::string>) {
    if (subidx != 0) {
      throw std::runtime_error("Expecting subidx == 0");
    } // Unused for flat trees at least
    x = static_cast<char *>(leaf->GetValuePointer());
  } else {
    x = leaf->GetTypedValue<T>(subidx);
  }
}

//----------------------------------------------------------------------
template <class T> T Proxy<T>::GetValueFlat() const {
  assert(fTree);

  // Valid cached or systematically-shifted value
  if (fEntry == fTree->GetReadEntry()) {
    return static_cast<T>(fVal);
  }
  fEntry = fTree->GetReadEntry();

  if (!fLeaf) {
    const std::string sname = StripSubscripts(fName);
    // In a flat tree the branch and leaf have the same name, and this is
    // quicker than the naive TTree::GetLeaf()
    fBranch = fTree->GetBranch(sname.c_str());
    fLeaf = fBranch ? fBranch->GetLeaf(sname.c_str()) : 0;

    // maybe the user wants to test if their branch is missing and do some other
    // action instead?
    if (!fLeaf) {
      throw caf::MissingBranchException("BasicTypesProxy", sname,
                                        fTree->GetName());
    }

    if (fName.find("..idx") == std::string::npos &&
        fName.find("..length") == std::string::npos) {
      SRBranchRegistry::AddBranch(sname);
    }
  }

  fBranch->GetEntry(fEntry);

  GetTypedValueWrapper(fLeaf, fVal, static_cast<int>(fBase + fOffset));

  return static_cast<T>(fVal);
}

template <class T>
void EvalInstanceWrapper(std::unique_ptr<TTreeFormula> &ttf, T &x) {
  static_assert(std::is_integral_v<T> || std::is_floating_point_v<T> ||
                    std::is_same_v<T, std::string>,
                "Can only TTreeFormula::EvalInstance on a integral, floating "
                "point, or std::string type.");
  if constexpr (std::is_integral_v<T>) {
    x = static_cast<T>(ttf->EvalInstance<Long64_t>(0));
  } else if constexpr (std::is_floating_point_v<T>) {
    x = static_cast<T>(ttf->EvalInstance<Double_t>(0));
  } else if constexpr (std::is_same_v<T, std::string>) {
    x = ttf->EvalStringInstance(0);
  }
}

//----------------------------------------------------------------------
template <class T> T Proxy<T>::GetValueNested() const {
  assert(fTree);

  // Valid cached or systematically-shifted value
  if (fEntry == fTree->GetReadEntry()) {
    return static_cast<T>(fVal);
  }
  fEntry = fTree->GetReadEntry();

  // First time calling, set up the branches etc
  if (!fTTF) {
    SRBranchRegistry::AddBranch(fName);

    // Leaves are attached to the TTF, must keep it
    fTTF = std::make_unique<TTreeFormula>(("TTFProxy-" + fName).c_str(),
                                          fName.c_str(), fTree);
    fLeafInfo = fTTF->GetLeafInfo(0); // Can fail (for a regular branch?)
    fLeaf = fTTF->GetLeaf(0);
    fBranch = fLeaf->GetBranch();

    if (!fLeaf || !fBranch) {
      std::stringstream ss;
      ss << "Couldn't find " << fName << " in tree. Abort.";
      throw std::runtime_error(ss.str());
    }

    // TODO - parsing the array indices out sucks - pass in as an int somehow
    const size_t open_idx = fName.find('[');
    // Do we have exactly one set of [] in the name?
    if (open_idx != std::string::npos && open_idx == fName.rfind('[')) {
      const size_t close_idx = fName.find(']');

      std::string numPart =
          fName.substr(open_idx + 1, close_idx - open_idx - 1);
      fSubIdx = atoi(numPart.c_str());
    }
  }

  if (fLeafInfo) {
    // Using TTreeFormula always works, and is sometimes necessary

    fTTF->GetNdata(); // for some reason this is necessary for fTTF to work
                      // in all cases.

    EvalInstanceWrapper(fTTF, fVal);
  } else {
    // But when this is possible the hope is it might be faster

    if (fBranch->GetReadEntry() != fEntry) {
      fBranch->GetEntry(fEntry);
    }

    // This check is much quicker than what CheckIndex() does, which winds up
    // calling a TTF, but I can't figure out a safe way to automatically
    // elide that check.
    if (fSubIdx > fLeaf->GetLen()) {
      std::stringstream ss;
      ss << fName << " out of range (" << fName
         << ".size() == " << fLeaf->GetLen() << "). Aborting.";
      throw std::runtime_error(ss.str());
    }

    GetTypedValueWrapper(fLeaf, fVal, fSubIdx);
  }

  return static_cast<T>(fVal);
}

//----------------------------------------------------------------------
template <class T> Proxy<T> &Proxy<T>::operator=(T x) {
  if (SRProxySystController::InTransaction())
    SRProxySystController::Backup(*this);
  fVal = x;

  switch (fType) {
  case kNested: {
    fEntry = fTree->GetReadEntry();
    break;
  }
  case kFlat: {
    fEntry = fTree->GetReadEntry();
    break;
  }
  case kCopiedRecord: {
    break;
  }
  default: {
    std::stringstream ss;
    ss << "Proxy<T>::operator= called for invalid fType: " << fType;
    throw std::runtime_error(ss.str());
  }
  }

  return *this;
}

//----------------------------------------------------------------------
template <class T> Proxy<T> &Proxy<T>::operator+=(T x) {
  if constexpr (!std::is_same_v<T, bool>) {
    // Do it this way to re-use the systematics logic in operator=
    // todo: should this be GetValueChecked()?  (otherwise we silently += a
    // NaN/inf)
    *this = T(GetValue() + x);
  } else {
    throw std::runtime_error("Proxy<bool>::operator+=() is meaningless");
  }

  return *this;
}

//----------------------------------------------------------------------
template <class T> Proxy<T> &Proxy<T>::operator-=(T x) {
  if constexpr (std::is_same_v<T, std::string>) {
    throw std::runtime_error("Proxy<std::string>::operator-=() is meaningless");
  } else if constexpr (std::is_same_v<T, bool>) {
    throw std::runtime_error("Proxy<bool>::operator-=() is meaningless");
  } else {
    // Do it this way to re-use the systematics logic in operator=
    *this = T(GetValue() - x);
  }

  return *this;
}

//----------------------------------------------------------------------
template <class T> Proxy<T> &Proxy<T>::operator*=(T x) {
  if constexpr (std::is_same_v<T, std::string>) {
    throw std::runtime_error("Proxy<std::string>::operator*=() is meaningless");
  } else if constexpr (std::is_same_v<T, bool>) {
    throw std::runtime_error("Proxy<bool>::operator*=() is meaningless");
  } else {
    // Do it this way to re-use the systematics logic in operator=
    // todo: should this be GetValueChecked()?  (otherwise we silently *= a
    // NaN/inf)
    *this = T(GetValue() * x);
  }

  return *this;
}

//----------------------------------------------------------------------
template <class T> void Proxy<T>::CheckEquals(const T &x) const {
  if (!AreEqual(GetValue(), x)) {
    std::cout << fName << " differs: " << GetValue() << " vs " << x
              << std::endl;
  }
}

} // namespace caf
