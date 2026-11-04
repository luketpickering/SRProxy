#include "SRProxy/BasicTypesProxy.txx"

namespace caf {
std::vector<std::unique_ptr<Restorer>> SRProxySystController::fRestorers;

long long SRProxySystController::fGeneration = 0;

std::set<std::string> SRBranchRegistry::fgBranches;

void SRBranchRegistry::Print(bool abbrev) {
  std::string prev;
  for (std::string b : fgBranches) {
    if (abbrev) {
      size_t cutto = 0;
      for (size_t i = 0; i < std::min(b.size(), prev.size()); ++i) {
        if (b[i] != prev[i]) {
          break;
        }
        if (b[i] == '.') {
          cutto = i;
        }
      }
      prev = b;
      for (size_t i = 0; i < cutto; ++i) {
        b[i] = ' ';
      }
    }
    std::cout << b << std::endl;
  }
}

void SRBranchRegistry::ToFile(const std::string &fname) {
  std::ofstream fout(fname);
  for (const std::string &b : fgBranches) {
    fout << b << std::endl;
  }
}

CAFType GetCAFType(TTree *tr) {
  if (!tr) {
    return kCopiedRecord;
  }

  // Allow user to override automatic CAF type detection if necessary
  const char *alias = tr->GetAlias("srproxy_metadata_caftype_override");
  if (alias) {
    if (alias == "nested"s) {
      return kNested;
    }
    if (alias == "flat"s) {
      return kFlat;
    }
  }

  if (tr->GetNbranches() > 1) {
    return kFlat;
  }
  return kNested;
}

int NSubscripts(const std::string &name) {
  return static_cast<int>(std::count(name.begin(), name.end(), '['));
}

ArrayVectorProxyBase::ArrayVectorProxyBase(TTree *tr, const std::string &name,
                                           bool isNestedContainer,
                                           const long &base, int offset,
                                           const Lineage *parent)
    : Lineage(parent), fTree(tr), fName(name),
      fIsNestedContainer(isNestedContainer), fType(GetCAFType(tr)), fBase(base),
      fOffset(offset), fIdxP(nullptr), fIdx(0) {}

void ArrayVectorProxyBase::EnsureIdxP() const {
  if (fIdxP) {
    return;
  }

  // Only used for flat trees. For single-tree, only needed for objects not
  // at top-level.
  if (fType == kFlat && NSubscripts(fName) > 0) {
    fIdxP = std::make_unique<Proxy<long long>>(fTree, IndexField(), fBase,
                                               fOffset, nullptr);
  }
}

void ArrayVectorProxyBase::CheckIndex(size_t i, size_t size) const {
  // This is the only way to get good error messages. But it also seems like
  // the call to size() here is necessary in Nested mode to trigger some
  // side-effect within ROOT, otherwise we get some bogus index out-of-range
  // crashes.
  if (i >= size) {
    std::stringstream ss;
    ss << fName << "[" << static_cast<signed>(i) << "] out of range (" << fName
       << ".size() == " << size << "). Aborting.";
    throw std::runtime_error(ss.str());
  }
}

std::string VectorProxyBase::NName() const {
  const size_t idx = fName.rfind('.');
  if (idx != std::string::npos) {
    // foo.bar.baz -> foo.bar.nbaz
    return fName.substr(0, idx) + ".n" + fName.substr(idx + 1);
  } else {
    // maybe the CAF is structured so this branch is at top level.
    // then it should just be "n" + the branch name
    return "n" + fName;
  }
}

std::string VectorProxyBase::LengthField() const {
  if (fType == kFlat) {
    return fName + "..length";
  }

  // Counts exist, but with non-systematic names
  if (fName == "rec.me.trkkalman") {
    return "rec.me.nkalman";
  }
  if (fName == "rec.me.trkdiscrete") {
    return "rec.me.ndiscrete";
  }
  if (fName == "rec.me.trkcosmic") {
    return "rec.me.ncosmic";
  }
  if (fName == "rec.me.trkbpf") {
    return "rec.me.nbpf";
  }

  // foo.bar.baz -> foo.bar.nbaz
  const std::string nname = NName();

  if (!fTree) {
    return nname; // doesn't matter if leaf exists or not
  }

  int olderr = gErrorIgnoreLevel;
  gErrorIgnoreLevel = 99999999;
  TTreeFormula ttf(("TTFProxySize-" + fName).c_str(), nname.c_str(), fTree);
  TString junks = nname.c_str();
  int junki;
  const int def = ttf.DefinedVariable(junks, junki);
  gErrorIgnoreLevel = olderr;

  if (def >= 0) {
    return nname;
  }

  // Otherwise fallback and warn (this is on the first time we're accessed)

  // foo.bar.baz -> foo.bar.@baz.size()
  const size_t idx = fName.rfind('.');
  const std::string ret =
      fName.substr(0, idx + 1) + "@" + fName.substr(idx + 1) + ".size()";

  // Don't emit the same warning more than once
  static std::set<std::string> already;

  const std::string key = StripSubscripts(NName());
  if (already.count(key) == 0) {
    already.insert(key);
    std::cout << std::endl;
    std::cout << "Warning: field '" << key << "' does not exist in file. "
              << "Falling back to '" << StripSubscripts(ret)
              << "' which is less efficient. "
              << "Consider updating StandardRecord to include '" << key << "'."
              << std::endl;
    std::cout << std::endl;
  }

  return ret;
}

std::string ArrayVectorProxyBase::IndexField() const {
  if (fType == kFlat) {
    return fName + "..idx";
  }
  std::stringstream ss;
  ss << "ArrayVectorProxyBase::IndexField called for invalid fType: " << fType;
  throw std::runtime_error(ss.str());
}

std::string ArrayVectorProxyBase::Subscript(size_t i) const {
  // Only have to do the at() business for the nested case for subscripts
  // from the 3rd one on
  if (fType != kNested || NSubscripts(fName) < 2) {
    return SubName() + "[" + std::to_string(i) + "]";
  }

  const size_t idx = fName.rfind('.'); // for nested name == subname

  return fName.substr(0, idx) + ".@" + fName.substr(idx + 1) + ".at(" +
         std::to_string(i) + ")";
}

std::string ArrayVectorProxyBase::SubName() const {
  // Nested containers would have the same name for length and idx at each
  // level, which is bad, so their names are uniquified.
  if (fType == kFlat && fIsNestedContainer) {
    return fName + ".elems";
  } else {
    return fName;
  }
}

bool ArrayVectorProxyBase::TreeHasLeaf(TTree *tr,
                                       const std::string &name) const {
  return tr->GetLeaf(name.c_str());
}

VectorProxyBase::VectorProxyBase(TTree *tr, const std::string &name,
                                 bool isNestedContainer, const long &base,
                                 int offset, const Lineage *parent)
    : ArrayVectorProxyBase(tr, name, isNestedContainer, base, offset, parent),
      fSize(nullptr) {}

void VectorProxyBase::EnsureSizeExists() const {
  if (fSize) {
    return;
  }

  fSize = std::make_unique<Proxy<int>>(fTree, LengthField(), fBase, fOffset,
                                       nullptr);
}

size_t VectorProxyBase::size() const {
  EnsureSizeExists();
  return *fSize;
}

bool VectorProxyBase::empty() const { return size() == 0; }

void VectorProxyBase::resize(size_t i) {
  EnsureSizeExists();
  *fSize = static_cast<int>(i);
}

// Enumerate all the variants we expect
template class Proxy<char>;
template class Proxy<short>;
template class Proxy<int>;
template class Proxy<long>;
template class Proxy<long long>;

template class Proxy<unsigned char>;
template class Proxy<unsigned short>;
template class Proxy<unsigned int>;
template class Proxy<unsigned long>;
template class Proxy<unsigned long long>;

template class Proxy<float>;
template class Proxy<double>;
template class Proxy<long double>;

template class Proxy<bool>;

template class Proxy<std::string>;

} // namespace caf
